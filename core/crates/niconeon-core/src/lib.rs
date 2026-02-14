use std::collections::{HashMap, HashSet};
use std::env;

use anyhow::{Context, Result};
use niconeon_domain::{CommentEvent, CommentSource};
use niconeon_filter::{FilterEngine, FilterError};
use niconeon_protocol::{
    AddNgUserParams, AddNgUserResult, AddRegexFilterParams, AddRegexFilterResult, JsonRpcRequest,
    JsonRpcResponse, ListFiltersResult, OpenVideoParams, OpenVideoResult, PingResult,
    PlaybackTickBatchParams, PlaybackTickBatchResult, PlaybackTickSample, RemoveNgUserParams,
    RemoveNgUserResult, RemoveRegexFilterParams, RemoveRegexFilterResult, SetRuntimeProfileParams,
    SetRuntimeProfileResult, UndoLastNgParams, UndoLastNgResult,
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

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum RuntimeProfile {
    High,
    Balanced,
    LowSpec,
}

impl RuntimeProfile {
    fn from_str(value: &str) -> Option<Self> {
        match value.trim().to_ascii_lowercase().as_str() {
            "high" => Some(Self::High),
            "balanced" => Some(Self::Balanced),
            "low_spec" | "lowspec" => Some(Self::LowSpec),
            _ => None,
        }
    }

    fn as_str(self) -> &'static str {
        match self {
            Self::High => "high",
            Self::Balanced => "balanced",
            Self::LowSpec => "low_spec",
        }
    }
}

#[derive(Debug, Clone)]
struct RuntimeProfileConfig {
    profile: RuntimeProfile,
    target_fps: u32,
    max_emit_per_tick: usize,
    coalesce_same_content: bool,
}

impl RuntimeProfileConfig {
    fn defaults() -> Self {
        Self::for_profile(RuntimeProfile::Balanced)
    }

    fn for_profile(profile: RuntimeProfile) -> Self {
        match profile {
            RuntimeProfile::High => Self {
                profile,
                target_fps: 30,
                max_emit_per_tick: 0,
                coalesce_same_content: false,
            },
            RuntimeProfile::Balanced => Self {
                profile,
                target_fps: 30,
                max_emit_per_tick: 96,
                coalesce_same_content: false,
            },
            RuntimeProfile::LowSpec => Self {
                profile,
                target_fps: 30,
                max_emit_per_tick: 48,
                coalesce_same_content: true,
            },
        }
    }

    fn apply_overrides(
        &mut self,
        target_fps: Option<u32>,
        max_emit_per_tick: Option<usize>,
        coalesce_same_content: Option<bool>,
    ) {
        if let Some(value) = target_fps {
            self.target_fps = value.clamp(10, 120);
        }
        if let Some(value) = max_emit_per_tick {
            self.max_emit_per_tick = value.min(2000);
        }
        if let Some(value) = coalesce_same_content {
            self.coalesce_same_content = value;
        }
    }
}

pub struct AppCore<F> {
    store: Store,
    fetcher: F,
    filter_engine: FilterEngine,
    sessions: HashMap<String, Session>,
    last_undo: Option<UndoState>,
    runtime_profile: RuntimeProfileConfig,
}

impl<F: CommentFetcher> AppCore<F> {
    pub fn new(store: Store, fetcher: F) -> Result<Self> {
        let ng_users = store.list_ng_users().context("load ng users")?;
        let regex_filters = store.list_regex_filters().context("load regex filters")?;
        let filter_engine = FilterEngine::new(ng_users, regex_filters)
            .map_err(|e| anyhow::anyhow!(e.to_string()))?;

        Ok(Self {
            store,
            fetcher,
            filter_engine,
            sessions: HashMap::new(),
            last_undo: None,
            runtime_profile: RuntimeProfileConfig::defaults(),
        })
    }

    pub fn handle_request(&mut self, req: JsonRpcRequest) -> JsonRpcResponse {
        let id = req.id.clone();
        let result: Result<serde_json::Value> = match req.method.as_str() {
            "ping" => self.ping().and_then(to_json_value),
            "open_video" => self.open_video(req.params).and_then(to_json_value),
            "playback_tick_batch" => self.playback_tick_batch(req.params).and_then(to_json_value),
            "add_ng_user" => self.add_ng_user(req.params).and_then(to_json_value),
            "remove_ng_user" => self.remove_ng_user(req.params).and_then(to_json_value),
            "undo_last_ng" => self.undo_last_ng(req.params).and_then(to_json_value),
            "add_regex_filter" => self.add_regex_filter(req.params).and_then(to_json_value),
            "remove_regex_filter" => self.remove_regex_filter(req.params).and_then(to_json_value),
            "list_filters" => self.list_filters().and_then(to_json_value),
            "set_runtime_profile" => self
                .set_runtime_profile(req.params)
                .and_then(to_json_value),
            _ => {
                return JsonRpcResponse::failure(
                    id,
                    -32601,
                    format!("method not found: {}", req.method),
                );
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

        let (comments, source) = if synthetic_comment_mode_enabled() {
            (
                generate_synthetic_comments_ramp(&params.video_id),
                CommentSource::Network,
            )
        } else {
            match self.fetcher.fetch_comments(&params.video_id) {
                Ok(comments) => {
                    self.store
                        .save_comment_cache(&params.video_id, &comments)
                        .context("save comment cache")?;
                    (comments, CommentSource::Network)
                }
                Err(err) => {
                    eprintln!("comment fetch failed for {}: {err:#}", params.video_id);
                    match self
                        .store
                        .load_comment_cache(&params.video_id)
                        .context("load cache")?
                    {
                        Some(cached) => (cached, CommentSource::Cache),
                        None => (Vec::new(), CommentSource::None),
                    }
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

    fn playback_tick_batch(
        &mut self,
        params: serde_json::Value,
    ) -> Result<PlaybackTickBatchResult> {
        let params: PlaybackTickBatchParams = parse_params(params)?;
        let session = self
            .sessions
            .get_mut(&params.session_id)
            .with_context(|| format!("unknown session: {}", params.session_id))?;

        let mut emit_comments = Vec::new();
        let filter_engine = &self.filter_engine;
        for tick in &params.ticks {
            emit_comments.extend(Self::apply_playback_tick(session, tick, filter_engine));
        }

        let (emit_comments, coalesced_comments) = if self.runtime_profile.coalesce_same_content {
            Self::coalesce_emitted_comments(emit_comments)
        } else {
            (emit_comments, 0)
        };

        let (emit_comments, dropped_comments, emit_over_budget) =
            Self::apply_emit_budget(emit_comments, self.runtime_profile.max_emit_per_tick);

        Ok(PlaybackTickBatchResult {
            emit_comments,
            processed_ticks: params.ticks.len(),
            last_position_ms: session.last_position_ms,
            dropped_comments,
            coalesced_comments,
            emit_over_budget,
        })
    }

    fn coalesce_emitted_comments(comments: Vec<CommentEvent>) -> (Vec<CommentEvent>, usize) {
        if comments.is_empty() {
            return (comments, 0);
        }

        let mut seen = HashSet::<(i64, String, String)>::new();
        let mut output = Vec::with_capacity(comments.len());
        let mut dropped = 0usize;
        for comment in comments {
            let key = (
                comment.at_ms,
                comment.user_id.clone(),
                comment.text.clone(),
            );
            if seen.insert(key) {
                output.push(comment);
            } else {
                dropped += 1;
            }
        }
        (output, dropped)
    }

    fn apply_emit_budget(
        mut comments: Vec<CommentEvent>,
        max_emit_per_tick: usize,
    ) -> (Vec<CommentEvent>, usize, bool) {
        if max_emit_per_tick == 0 || comments.len() <= max_emit_per_tick {
            return (comments, 0, false);
        }

        let dropped = comments.len().saturating_sub(max_emit_per_tick);
        comments.truncate(max_emit_per_tick);
        (comments, dropped, true)
    }

    fn apply_playback_tick(
        session: &mut Session,
        tick: &PlaybackTickSample,
        filter_engine: &FilterEngine,
    ) -> Vec<CommentEvent> {
        if tick.is_seek || tick.position_ms < session.last_position_ms {
            session.cursor = cursor_for_position(&session.comments, tick.position_ms);
            session.last_position_ms = tick.position_ms;
            return Vec::new();
        }

        if tick.paused {
            session.last_position_ms = tick.position_ms;
            return Vec::new();
        }

        let from_ms = session.last_position_ms;
        let to_ms = tick.position_ms;

        let mut emit_comments = Vec::new();
        while session.cursor < session.comments.len() {
            let c = &session.comments[session.cursor];
            if c.at_ms > to_ms {
                break;
            }

            if c.at_ms > from_ms && !filter_engine.should_hide(c) {
                emit_comments.push(c.clone());
            }
            session.cursor += 1;
        }

        session.last_position_ms = to_ms;
        emit_comments
    }

    fn add_ng_user(&mut self, params: serde_json::Value) -> Result<AddNgUserResult> {
        let params: AddNgUserParams = parse_params(params)?;

        let applied = self.filter_engine.add_ng_user(&params.user_id);
        let _ = self
            .store
            .add_ng_user(&params.user_id)
            .context("save ng user")?;

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

    fn remove_ng_user(&mut self, params: serde_json::Value) -> Result<RemoveNgUserResult> {
        let params: RemoveNgUserParams = parse_params(params)?;

        let removed_db = self
            .store
            .remove_ng_user(&params.user_id)
            .context("delete ng user")?;
        let removed_mem = self.filter_engine.remove_ng_user(&params.user_id);

        if removed_db || removed_mem {
            if self
                .last_undo
                .as_ref()
                .map(|undo| undo.user_id == params.user_id)
                .unwrap_or(false)
            {
                self.last_undo = None;
            }
        }

        Ok(RemoveNgUserResult {
            removed: removed_db || removed_mem,
            user_id: params.user_id,
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
        let _ = self
            .store
            .remove_ng_user(&user_id)
            .context("remove ng user")?;
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

    fn remove_regex_filter(
        &mut self,
        params: serde_json::Value,
    ) -> Result<RemoveRegexFilterResult> {
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

    fn set_runtime_profile(
        &mut self,
        params: serde_json::Value,
    ) -> Result<SetRuntimeProfileResult> {
        let params: SetRuntimeProfileParams = parse_params(params)?;
        let profile = RuntimeProfile::from_str(&params.profile)
            .ok_or_else(|| anyhow::anyhow!("invalid runtime profile: {}", params.profile))?;
        let mut config = RuntimeProfileConfig::for_profile(profile);
        config.apply_overrides(
            params.target_fps,
            params.max_emit_per_tick,
            params.coalesce_same_content,
        );
        self.runtime_profile = config.clone();

        Ok(SetRuntimeProfileResult {
            profile: self.runtime_profile.profile.as_str().to_string(),
            target_fps: self.runtime_profile.target_fps,
            max_emit_per_tick: self.runtime_profile.max_emit_per_tick,
            coalesce_same_content: self.runtime_profile.coalesce_same_content,
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

fn synthetic_comment_mode_enabled() -> bool {
    match env::var("NICONEON_SYNTHETIC_COMMENTS") {
        Ok(v) => v.eq_ignore_ascii_case("ramp"),
        Err(_) => false,
    }
}

fn env_i64(name: &str, default: i64) -> i64 {
    env::var(name)
        .ok()
        .and_then(|v| v.parse::<i64>().ok())
        .unwrap_or(default)
}

fn generate_synthetic_comments_ramp(video_id: &str) -> Vec<CommentEvent> {
    let duration_sec = env_i64("NICONEON_SYNTHETIC_DURATION_SEC", 120).clamp(1, 3600);
    let base_per_sec = env_i64("NICONEON_SYNTHETIC_BASE_PER_SEC", 1).clamp(1, 500);
    let ramp_per_sec = env_i64("NICONEON_SYNTHETIC_RAMP_PER_SEC", 1).clamp(0, 100);
    let max_per_sec = env_i64("NICONEON_SYNTHETIC_MAX_PER_SEC", 160).clamp(1, 2000);
    let user_span = env_i64("NICONEON_SYNTHETIC_USER_SPAN", 200).clamp(1, 100000);

    let mut comments = Vec::new();
    comments.reserve((duration_sec * (base_per_sec + max_per_sec) / 2) as usize);

    for sec in 0..duration_sec {
        let per_sec = (base_per_sec + sec * ramp_per_sec).min(max_per_sec);
        for idx in 0..per_sec {
            let at_ms = sec * 1000 + ((idx * 1000) / per_sec);
            comments.push(CommentEvent {
                comment_id: format!("{}-dummy-{}-{}", video_id, sec, idx),
                at_ms,
                user_id: format!("dummy-user-{}", (sec * 31 + idx) % user_span),
                text: format!("dummy comment {}-{} / {}cps", sec, idx, per_sec),
            });
        }
    }

    comments
}

#[cfg(test)]
mod tests {
    use std::cell::RefCell;

    use anyhow::Result;
    use niconeon_domain::CommentEvent;
    use niconeon_protocol::{
        JsonRpcRequest, OpenVideoParams, PlaybackTickBatchParams, PlaybackTickSample,
    };
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
            method: "playback_tick_batch".to_string(),
            params: serde_json::to_value(PlaybackTickBatchParams {
                session_id,
                ticks: vec![PlaybackTickSample {
                    position_ms: 150,
                    paused: false,
                    is_seek: false,
                }],
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
        let processed_ticks = res
            .result
            .as_ref()
            .and_then(|v| v.get("processed_ticks"))
            .and_then(|v| v.as_u64())
            .expect("processed ticks");
        let last_position_ms = res
            .result
            .as_ref()
            .and_then(|v| v.get("last_position_ms"))
            .and_then(|v| v.as_i64())
            .expect("last position");

        assert_eq!(emitted.len(), 1);
        assert_eq!(processed_ticks, 1);
        assert_eq!(last_position_ms, 150);
        assert_eq!(
            emitted[0].get("comment_id").and_then(|v| v.as_str()),
            Some("a")
        );
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

    #[test]
    fn runtime_profile_caps_emit_comments() {
        let mut comments = Vec::new();
        for idx in 0..20 {
            comments.push(CommentEvent {
                comment_id: format!("c{idx}"),
                at_ms: 100 + idx as i64,
                user_id: format!("u{}", idx % 3),
                text: format!("comment-{idx}"),
            });
        }

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

        let set_profile = JsonRpcRequest {
            jsonrpc: "2.0".to_string(),
            id: json!(2),
            method: "set_runtime_profile".to_string(),
            params: json!({
                "profile": "low_spec",
                "max_emit_per_tick": 5,
                "coalesce_same_content": false
            }),
        };
        let _ = app.handle_request(set_profile);

        let tick = JsonRpcRequest {
            jsonrpc: "2.0".to_string(),
            id: json!(3),
            method: "playback_tick_batch".to_string(),
            params: json!({
                "session_id": session_id,
                "ticks": [{
                    "position_ms": 300,
                    "paused": false,
                    "is_seek": false
                }]
            }),
        };
        let res = app.handle_request(tick);
        let emitted = res
            .result
            .as_ref()
            .and_then(|v| v.get("emit_comments"))
            .and_then(|v| v.as_array())
            .expect("emit comments");
        let dropped = res
            .result
            .as_ref()
            .and_then(|v| v.get("dropped_comments"))
            .and_then(|v| v.as_u64())
            .expect("dropped comments");
        let over_budget = res
            .result
            .as_ref()
            .and_then(|v| v.get("emit_over_budget"))
            .and_then(|v| v.as_bool())
            .expect("emit over budget");

        assert_eq!(emitted.len(), 5);
        assert_eq!(dropped, 15);
        assert!(over_budget);
    }

    #[test]
    fn runtime_profile_can_coalesce_comments() {
        let comments = vec![
            CommentEvent {
                comment_id: "a".to_string(),
                at_ms: 100,
                user_id: "u1".to_string(),
                text: "same".to_string(),
            },
            CommentEvent {
                comment_id: "b".to_string(),
                at_ms: 100,
                user_id: "u1".to_string(),
                text: "same".to_string(),
            },
            CommentEvent {
                comment_id: "c".to_string(),
                at_ms: 120,
                user_id: "u2".to_string(),
                text: "different".to_string(),
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

        let set_profile = JsonRpcRequest {
            jsonrpc: "2.0".to_string(),
            id: json!(2),
            method: "set_runtime_profile".to_string(),
            params: json!({
                "profile": "low_spec",
                "max_emit_per_tick": 0,
                "coalesce_same_content": true
            }),
        };
        let _ = app.handle_request(set_profile);

        let tick = JsonRpcRequest {
            jsonrpc: "2.0".to_string(),
            id: json!(3),
            method: "playback_tick_batch".to_string(),
            params: serde_json::to_value(PlaybackTickBatchParams {
                session_id,
                ticks: vec![PlaybackTickSample {
                    position_ms: 200,
                    paused: false,
                    is_seek: false,
                }],
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
        let coalesced = res
            .result
            .as_ref()
            .and_then(|v| v.get("coalesced_comments"))
            .and_then(|v| v.as_u64())
            .expect("coalesced comments");

        assert_eq!(emitted.len(), 2);
        assert_eq!(coalesced, 1);
    }

    #[test]
    fn runtime_profile_rejects_unknown_profile() {
        let store = Store::open_memory().expect("store");
        let fetcher = MockFetcher {
            data: RefCell::new(Ok(Vec::new())),
        };
        let mut app = AppCore::new(store, fetcher).expect("app");

        let req = JsonRpcRequest {
            jsonrpc: "2.0".to_string(),
            id: json!(1),
            method: "set_runtime_profile".to_string(),
            params: json!({
                "profile": "unknown"
            }),
        };
        let res = app.handle_request(req);
        assert!(res.error.is_some());
    }

    #[test]
    fn legacy_playback_tick_method_is_not_supported() {
        let store = Store::open_memory().expect("store");
        let fetcher = MockFetcher {
            data: RefCell::new(Ok(Vec::new())),
        };
        let mut app = AppCore::new(store, fetcher).expect("app");

        let req = JsonRpcRequest {
            jsonrpc: "2.0".to_string(),
            id: json!(1),
            method: "playback_tick".to_string(),
            params: json!({}),
        };
        let res = app.handle_request(req);
        assert!(res.error.is_some());
        let message = res.error.as_ref().map(|e| e.message.as_str()).unwrap_or("");
        assert!(message.contains("method not found"));
    }

    #[test]
    fn remove_ng_user_updates_filter_list() {
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
        let _ = app.handle_request(add);

        let remove = JsonRpcRequest {
            jsonrpc: "2.0".to_string(),
            id: json!(2),
            method: "remove_ng_user".to_string(),
            params: json!({ "user_id": "u1" }),
        };
        let remove_res = app.handle_request(remove);
        let removed = remove_res
            .result
            .as_ref()
            .and_then(|v| v.get("removed"))
            .and_then(|v| v.as_bool())
            .expect("removed");
        assert!(removed);

        let list = JsonRpcRequest {
            jsonrpc: "2.0".to_string(),
            id: json!(3),
            method: "list_filters".to_string(),
            params: json!({}),
        };
        let list_res = app.handle_request(list);
        let ng_users = list_res
            .result
            .as_ref()
            .and_then(|v| v.get("ng_users"))
            .and_then(|v| v.as_array())
            .expect("ng users");
        assert!(ng_users.is_empty());
    }
}
