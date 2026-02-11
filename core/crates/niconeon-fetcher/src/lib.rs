use anyhow::{anyhow, Context, Result};
use niconeon_domain::CommentEvent;
use reqwest::blocking::Client;
use reqwest::header::{HeaderMap, HeaderValue, CONTENT_TYPE, COOKIE, ORIGIN, REFERER, USER_AGENT};
use serde::Deserialize;

#[derive(Debug, Clone)]
pub struct NiconicoFetcher {
    client: Client,
    cookie: Option<String>,
}

impl NiconicoFetcher {
    pub fn new(cookie: Option<String>) -> Result<Self> {
        let client = Client::builder().build().context("build reqwest client")?;
        Ok(Self { client, cookie })
    }

    pub fn fetch_comments(&self, video_id: &str) -> Result<Vec<CommentEvent>> {
        let watch_url = format!("https://www.nicovideo.jp/watch/{video_id}?responseType=json");
        let watch: WatchResponse = self
            .client
            .get(watch_url)
            .header(USER_AGENT, "Mozilla/5.0 (Niconeon)")
            .send()
            .context("fetch watch response json")?
            .error_for_status()
            .context("watch response status")?
            .json()
            .context("parse watch response json")?;

        let nv = watch
            .data
            .response
            .comment
            .nv_comment
            .ok_or_else(|| anyhow!("nvComment section not found"))?;

        let req_body = NvThreadsRequest {
            thread_key: nv.thread_key,
            params: nv.params,
            additionals: serde_json::json!({}),
        };

        let endpoint = format!("{}/v1/threads", nv.server);
        let mut headers = HeaderMap::new();
        headers.insert(CONTENT_TYPE, HeaderValue::from_static("application/json"));
        headers.insert(ORIGIN, HeaderValue::from_static("https://www.nicovideo.jp"));
        headers.insert(REFERER, HeaderValue::from_static("https://www.nicovideo.jp/"));
        headers.insert("X-Frontend-Id", HeaderValue::from_static("6"));
        headers.insert("X-Frontend-Version", HeaderValue::from_static("0"));
        headers.insert("X-Niconico-Language", HeaderValue::from_static("ja-jp"));
        headers.insert(USER_AGENT, HeaderValue::from_static("Mozilla/5.0 (Niconeon)"));

        if let Some(cookie) = &self.cookie {
            headers.insert(COOKIE, HeaderValue::from_str(cookie).context("invalid cookie header")?);
        }

        let res: ThreadsResponse = self
            .client
            .post(endpoint)
            .headers(headers)
            .json(&req_body)
            .send()
            .context("fetch nvcomment threads")?
            .error_for_status()
            .context("nvcomment status")?
            .json()
            .context("parse nvcomment json")?;

        if res.meta.status != 200 {
            return Err(anyhow!("nvcomment meta status != 200"));
        }

        let mut comments = Vec::new();
        for thread in res.data.threads {
            for c in thread.comments {
                comments.push(CommentEvent {
                    comment_id: c.id,
                    at_ms: c.vpos_ms,
                    user_id: c.user_id.unwrap_or_else(|| "anonymous".to_string()),
                    text: c.body,
                });
            }
        }

        comments.sort_by_key(|c| c.at_ms);
        Ok(comments)
    }
}

#[derive(Debug, Deserialize)]
struct WatchResponse {
    data: WatchData,
}

#[derive(Debug, Deserialize)]
struct WatchData {
    response: WatchBody,
}

#[derive(Debug, Deserialize)]
struct WatchBody {
    comment: WatchComment,
}

#[derive(Debug, Deserialize)]
struct WatchComment {
    #[serde(rename = "nvComment")]
    nv_comment: Option<NvCommentData>,
}

#[derive(Debug, Deserialize)]
struct NvCommentData {
    server: String,
    #[serde(rename = "threadKey")]
    thread_key: String,
    params: NvCommentParams,
}

#[derive(Debug, Deserialize, serde::Serialize)]
struct NvCommentParams {
    targets: Vec<NvTarget>,
    language: String,
}

#[derive(Debug, Deserialize, serde::Serialize)]
struct NvTarget {
    id: String,
    fork: String,
}

#[derive(Debug, serde::Serialize)]
struct NvThreadsRequest {
    #[serde(rename = "threadKey")]
    thread_key: String,
    params: NvCommentParams,
    additionals: serde_json::Value,
}

#[derive(Debug, Deserialize)]
struct ThreadsResponse {
    meta: ThreadsMeta,
    data: ThreadsData,
}

#[derive(Debug, Deserialize)]
struct ThreadsMeta {
    status: i64,
}

#[derive(Debug, Deserialize)]
struct ThreadsData {
    threads: Vec<ThreadItem>,
}

#[derive(Debug, Deserialize)]
struct ThreadItem {
    comments: Vec<ThreadComment>,
}

#[derive(Debug, Deserialize)]
struct ThreadComment {
    id: String,
    #[serde(rename = "vposMs")]
    vpos_ms: i64,
    body: String,
    #[serde(rename = "userId")]
    user_id: Option<String>,
}
