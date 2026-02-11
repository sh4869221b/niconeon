use niconeon_domain::{CommentEvent, RegexFilter};
use serde::{Deserialize, Serialize};
use serde_json::Value;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct JsonRpcRequest {
    pub jsonrpc: String,
    pub id: Value,
    pub method: String,
    #[serde(default)]
    pub params: Value,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct JsonRpcResponse {
    pub jsonrpc: String,
    pub id: Value,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub result: Option<Value>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<JsonRpcError>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct JsonRpcError {
    pub code: i64,
    pub message: String,
}

impl JsonRpcResponse {
    pub fn success<T: Serialize>(id: Value, result: T) -> Self {
        Self {
            jsonrpc: "2.0".to_string(),
            id,
            result: Some(serde_json::to_value(result).expect("serialize result")),
            error: None,
        }
    }

    pub fn failure(id: Value, code: i64, message: impl Into<String>) -> Self {
        Self {
            jsonrpc: "2.0".to_string(),
            id,
            result: None,
            error: Some(JsonRpcError {
                code,
                message: message.into(),
            }),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct OpenVideoParams {
    pub video_path: String,
    pub video_id: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct OpenVideoResult {
    pub session_id: String,
    pub comment_source: String,
    pub total_comments: usize,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PlaybackTickParams {
    pub session_id: String,
    pub position_ms: i64,
    pub paused: bool,
    pub is_seek: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PlaybackTickResult {
    pub emit_comments: Vec<CommentEvent>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AddNgUserParams {
    pub user_id: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AddNgUserResult {
    pub applied: bool,
    pub undo_token: String,
    pub hidden_user_id: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RemoveNgUserParams {
    pub user_id: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RemoveNgUserResult {
    pub removed: bool,
    pub user_id: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct UndoLastNgParams {
    pub undo_token: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct UndoLastNgResult {
    pub restored: bool,
    pub user_id: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AddRegexFilterParams {
    pub pattern: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AddRegexFilterResult {
    pub filter_id: i64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RemoveRegexFilterParams {
    pub filter_id: i64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RemoveRegexFilterResult {
    pub removed: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ListFiltersResult {
    pub ng_users: Vec<String>,
    pub regex_filters: Vec<RegexFilter>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PingResult {
    pub ok: bool,
}
