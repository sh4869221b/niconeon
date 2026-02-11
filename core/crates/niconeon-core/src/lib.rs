use std::collections::HashMap;

use anyhow::{Context, Result};
use niconeon_domain::{CommentEvent, CommentSource};
use niconeon_filter::{FilterEngine, FilterError};
use niconeon_protocol::{
    AddNgUserParams, AddNgUserResult, AddRegexFilterParams, AddRegexFilterResult, JsonRpcRequest,
    JsonRpcResponse, ListFiltersResult, OpenVideoParams, OpenVideoResult, PingResult,
    PlaybackTickParams, PlaybackTickResult, RemoveRegexFilterParams, RemoveRegexFilterResult,
    UndoLastNgParams, UndoLastNgResult,
};
use niconeon_store::Store;
use uuid::Uuid;

pub trait CommentFetcher {
    fn fetch_comments(&self, video_id: &str) -> Result<Vec<CommentEvent>>;
}

impl CommentFetcher for niconeon_fetcher::NiconicoFetcher {
    fn fetch_comments(&self, video_id: &str) -> Result<Vec<CommentEvent>> {
        self.fetch_comments(video_id)
    }
}

#[derive(Debug, Clone)]
struct Session {
    comments: Vec<CommentEvent>,
    cursor: usize,
    last_position_ms: i64,
}

#[derive(Debug, Clone)]
struct UndoState {
    token: String,
    user_id: String,
}

pub struct AppCore<F> {
    store: Store,
    fetcher: F,
    filter_engine: FilterEngine,
    sessions: HashMap<String, Session>,
    last_undo: Option<UndoState>,
}

impl<F: CommentFetcher> AppCore<F> {
    pub fn new(store: Store, fetcher: F) -> Result<Self> {
        let ng_users = store.list_ng_users().context("load ng users")?;
        let regex_filters = store.list_regex_filters().context("load regex filters")?;
        let filter_engine =
            FilterEngine::new(ng_users, regex_filters).map_err(|e| anyhow::anyhow!(e.to_string()))?;

        Ok(Self {
            store,
            fetcher,
            filter_engine,
            sessions: HashMap::new(),
            last_undo: None,
        })
    }

    pub fn handle_request(&mut self, req: JsonRpcRequest) -> JsonRpcResponse {
        let id = req.id.clone();
        let result: Result<serde_json::Value> = match req.method.as_str() {
            "ping" => self.ping().and_then(to_json_value),
            "open_video" => self.open_video(req.params).and_then(to_json_value),
            "playback_tick" => self.playback_tick(req.params).and_then(to_json_value),
            "add_ng_user" => self.add_ng_user(req.params).and_then(to_json_value),
            "undo_last_ng" => self.undo_last_ng(req.params).and_then(to_json_value),
            "add_regex_filter" => self.add_regex_filter(req.params).and_then(to_json_value),
            "remove_regex_filter" => self.remove_regex_filter(req.params).and_then(to_json_value),
            "list_filters" => self.list_filters().and_then(to_json_value),
            _ => {
                return JsonRpcResponse::failure(id, -32601, format!("method not found: {}", req.method));
            }
        };

        match result {
            Ok(value) => JsonRpcResponse::success(id, value),
            Err(e) => JsonRpcResponse::failure(id, -32000, e.to_string()),
        }
    }

    fn ping(&self) -> Result<PingResult> {
        Ok(PingResult { ok: true })
    }

    fn open_video(&mut self, params: serde_json::Value) -> Result<OpenVideoResult> {
        let params: OpenVideoParams = parse_params(params)?;
        self.store
            .upsert_video_map(&params.video_path, &params.video_id)
            .context("save video mapping")?;

        let (comments, source) = match self.fetcher.fetch_comments(&params.video_id) {
            Ok(comments) => {
                self.store
                    .save_comment_cache(&params.video_id, &comments)
                    .context("save comment cache")?;
                (comments, CommentSource::Network)
            }
            Err(err) => {
                eprintln!("comment fetch failed for {}: {err:#}", params.video_id);
                match self.store.load_comment_cache(&params.video_id).context("load cache")? {
                    Some(cached) => (cached, CommentSource::Cache),
                    None => (Vec::new(), CommentSource::None),
                }
            }
        };

        let session_id = Uuid::new_v4().to_string();
        let cursor = cursor_for_position(&comments, 0);
        self.sessions.insert(
            session_id.clone(),
            Session {
                comments,
                cursor,
                last_position_ms: 0,
            },
        );

        let total = self
            .sessions
            .get(&session_id)
            .map(|s| s.comments.len())
            .unwrap_or_default();

        Ok(OpenVideoResult {
            session_id,
            comment_source: source.as_str().to_string(),
            total_comments: total,
        })
    }

    fn playback_tick(&mut self, params: serde_json::Value) -> Result<PlaybackTickResult> {
        let params: PlaybackTickParams = parse_params(params)?;
        let session = self
            .sessions
            .get_mut(&params.session_id)
            .with_context(|| format!("unknown session: {}", params.session_id))?;

        if params.is_seek || params.position_ms < session.last_position_ms {
            session.cursor = cursor_for_position(&session.comments, params.position_ms);
            session.last_position_ms = params.position_ms;
            return Ok(PlaybackTickResult {
                emit_comments: Vec::new(),
            });
        }

        if params.paused {
            session.last_position_ms = params.position_ms;
            return Ok(PlaybackTickResult {
                emit_comments: Vec::new(),
            });
        }

        let from_ms = session.last_position_ms;
        let to_ms = params.position_ms;

        let mut emit_comments = Vec::new();
        while session.cursor < session.comments.len() {
            let c = &session.comments[session.cursor];
            if c.at_ms > to_ms {
                break;
            }

            if c.at_ms > from_ms && !self.filter_engine.should_hide(c) {
                emit_comments.push(c.clone());
            }
            session.cursor += 1;
        }

        session.last_position_ms = to_ms;
        Ok(PlaybackTickResult { emit_comments })
    }

    fn add_ng_user(&mut self, params: serde_json::Value) -> Result<AddNgUserResult> {
        let params: AddNgUserParams = parse_params(params)?;

        let applied = self.filter_engine.add_ng_user(&params.user_id);
        let _ = self.store.add_ng_user(&params.user_id).context("save ng user")?;

        let undo_token = if applied {
            let token = Uuid::new_v4().to_string();
            self.last_undo = Some(UndoState {
                token: token.clone(),
                user_id: params.user_id.clone(),
            });
            token
        } else {
            String::new()
        };

        Ok(AddNgUserResult {
            applied,
            undo_token,
            hidden_user_id: params.user_id,
        })
    }

    fn undo_last_ng(&mut self, params: serde_json::Value) -> Result<UndoLastNgResult> {
        let params: UndoLastNgParams = parse_params(params)?;

        let Some(last) = &self.last_undo else {
            return Ok(UndoLastNgResult {
                restored: false,
                user_id: None,
            });
        };

        if last.token != params.undo_token {
            return Ok(UndoLastNgResult {
                restored: false,
                user_id: None,
            });
        }

        let user_id = last.user_id.clone();
        self.filter_engine.remove_ng_user(&user_id);
        let _ = self.store.remove_ng_user(&user_id).context("remove ng user")?;
        self.last_undo = None;

        Ok(UndoLastNgResult {
            restored: true,
            user_id: Some(user_id),
        })
    }

    fn add_regex_filter(&mut self, params: serde_json::Value) -> Result<AddRegexFilterResult> {
        let params: AddRegexFilterParams = parse_params(params)?;

        // Validate before writing persistent state.
        regex::Regex::new(&params.pattern)
            .map_err(|e| anyhow::anyhow!(FilterError::InvalidRegex(e.to_string()).to_string()))?;

        let filter = self
            .store
            .insert_regex_filter(&params.pattern)
            .context("insert regex filter")?;

        self.filter_engine
            .add_regex_filter(filter.clone())
            .map_err(|e| anyhow::anyhow!(e.to_string()))?;

        Ok(AddRegexFilterResult {
            filter_id: filter.filter_id,
        })
    }

    fn remove_regex_filter(&mut self, params: serde_json::Value) -> Result<RemoveRegexFilterResult> {
        let params: RemoveRegexFilterParams = parse_params(params)?;
        let removed_db = self
            .store
            .remove_regex_filter(params.filter_id)
            .context("delete regex filter")?;
        let removed_mem = self.filter_engine.remove_regex_filter(params.filter_id);

        Ok(RemoveRegexFilterResult {
            removed: removed_db || removed_mem,
        })
    }

    fn list_filters(&self) -> Result<ListFiltersResult> {
        Ok(ListFiltersResult {
            ng_users: self.filter_engine.list_ng_users(),
            regex_filters: self.filter_engine.list_regex_filters(),
        })
    }
}

fn parse_params<T: serde::de::DeserializeOwned>(value: serde_json::Value) -> Result<T> {
    serde_json::from_value(value).context("invalid params")
}

fn to_json_value<T: serde::Serialize>(value: T) -> Result<serde_json::Value> {
    serde_json::to_value(value).context("serialize result")
}

fn cursor_for_position(comments: &[CommentEvent], position_ms: i64) -> usize {
    comments.partition_point(|c| c.at_ms <= position_ms)
}

#[cfg(test)]
mod tests {
    use std::cell::RefCell;

    use anyhow::Result;
    use niconeon_domain::CommentEvent;
    use niconeon_protocol::{JsonRpcRequest, OpenVideoParams, PlaybackTickParams};
    use niconeon_store::Store;
    use serde_json::json;

    use super::{AppCore, CommentFetcher};

    struct MockFetcher {
        data: RefCell<Result<Vec<CommentEvent>, String>>,
    }

    impl CommentFetcher for MockFetcher {
        fn fetch_comments(&self, _video_id: &str) -> Result<Vec<CommentEvent>> {
            match self.data.borrow().as_ref() {
                Ok(v) => Ok(v.clone()),
                Err(e) => Err(anyhow::anyhow!(e.clone())),
            }
        }
    }

    fn open_video_req() -> JsonRpcRequest {
        JsonRpcRequest {
            jsonrpc: "2.0".to_string(),
            id: json!(1),
            method: "open_video".to_string(),
            params: serde_json::to_value(OpenVideoParams {
                video_path: "movie_sm9.mp4".to_string(),
                video_id: "sm9".to_string(),
            })
            .expect("params"),
        }
    }

    #[test]
    fn tick_emits_expected_window() {
        let comments = vec![
            CommentEvent {
                comment_id: "a".to_string(),
                at_ms: 100,
                user_id: "u1".to_string(),
                text: "one".to_string(),
            },
            CommentEvent {
                comment_id: "b".to_string(),
                at_ms: 200,
                user_id: "u2".to_string(),
                text: "two".to_string(),
            },
        ];

        let store = Store::open_memory().expect("store");
        let fetcher = MockFetcher {
            data: RefCell::new(Ok(comments)),
        };
        let mut app = AppCore::new(store, fetcher).expect("app");

        let open = app.handle_request(open_video_req());
        let session_id = open
            .result
            .as_ref()
            .and_then(|v| v.get("session_id"))
            .and_then(|v| v.as_str())
            .expect("session id")
            .to_string();

        let tick = JsonRpcRequest {
            jsonrpc: "2.0".to_string(),
            id: json!(2),
            method: "playback_tick".to_string(),
            params: serde_json::to_value(PlaybackTickParams {
                session_id,
                position_ms: 150,
                paused: false,
                is_seek: false,
            })
            .expect("tick params"),
        };

        let res = app.handle_request(tick);
        let emitted = res
            .result
            .as_ref()
            .and_then(|v| v.get("emit_comments"))
            .and_then(|v| v.as_array())
            .expect("emit comments");

        assert_eq!(emitted.len(), 1);
        assert_eq!(emitted[0].get("comment_id").and_then(|v| v.as_str()), Some("a"));
    }

    #[test]
    fn falls_back_to_cache() {
        let store = Store::open_memory().expect("store");
        let cached = vec![CommentEvent {
            comment_id: "c1".to_string(),
            at_ms: 42,
            user_id: "u1".to_string(),
            text: "cached".to_string(),
        }];
        store
            .save_comment_cache("sm9", &cached)
            .expect("save comment cache");

        let fetcher = MockFetcher {
            data: RefCell::new(Err("network error".to_string())),
        };
        let mut app = AppCore::new(store, fetcher).expect("app");
        let open = app.handle_request(open_video_req());

        let src = open
            .result
            .as_ref()
            .and_then(|v| v.get("comment_source"))
            .and_then(|v| v.as_str())
            .expect("source");
        let total = open
            .result
            .as_ref()
            .and_then(|v| v.get("total_comments"))
            .and_then(|v| v.as_u64())
            .expect("total");

        assert_eq!(src, "cache");
        assert_eq!(total, 1);
    }

    #[test]
    fn undo_only_latest_token() {
        let store = Store::open_memory().expect("store");
        let fetcher = MockFetcher {
            data: RefCell::new(Ok(Vec::new())),
        };
        let mut app = AppCore::new(store, fetcher).expect("app");

        let add = JsonRpcRequest {
            jsonrpc: "2.0".to_string(),
            id: json!(1),
            method: "add_ng_user".to_string(),
            params: json!({ "user_id": "u1" }),
        };
        let add_res = app.handle_request(add);
        let token = add_res
            .result
            .as_ref()
            .and_then(|v| v.get("undo_token"))
            .and_then(|v| v.as_str())
            .expect("undo token")
            .to_string();

        let bad_undo = JsonRpcRequest {
            jsonrpc: "2.0".to_string(),
            id: json!(2),
            method: "undo_last_ng".to_string(),
            params: json!({ "undo_token": "wrong" }),
        };
        let bad_res = app.handle_request(bad_undo);
        let restored = bad_res
            .result
            .as_ref()
            .and_then(|v| v.get("restored"))
            .and_then(|v| v.as_bool())
            .expect("restored");
        assert!(!restored);

        let good_undo = JsonRpcRequest {
            jsonrpc: "2.0".to_string(),
            id: json!(3),
            method: "undo_last_ng".to_string(),
            params: json!({ "undo_token": token }),
        };
        let good_res = app.handle_request(good_undo);
        let restored = good_res
            .result
            .as_ref()
            .and_then(|v| v.get("restored"))
            .and_then(|v| v.as_bool())
            .expect("restored");
        assert!(restored);
    }

    #[test]
    fn invalid_regex_is_rejected() {
        let store = Store::open_memory().expect("store");
        let fetcher = MockFetcher {
            data: RefCell::new(Ok(Vec::new())),
        };
        let mut app = AppCore::new(store, fetcher).expect("app");

        let req = JsonRpcRequest {
            jsonrpc: "2.0".to_string(),
            id: json!(1),
            method: "add_regex_filter".to_string(),
            params: json!({ "pattern": "(" }),
        };
        let res = app.handle_request(req);
        assert!(res.error.is_some());
    }
}
