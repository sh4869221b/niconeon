use chrono::{DateTime, Utc};
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct CommentEvent {
    pub comment_id: String,
    pub at_ms: i64,
    pub user_id: String,
    pub text: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct RegexFilter {
    pub filter_id: i64,
    pub pattern: String,
    pub created_at: DateTime<Utc>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct NgUser {
    pub user_id: String,
    pub created_at: DateTime<Utc>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum CommentSource {
    Network,
    Cache,
    None,
}

impl CommentSource {
    pub fn as_str(self) -> &'static str {
        match self {
            CommentSource::Network => "network",
            CommentSource::Cache => "cache",
            CommentSource::None => "none",
        }
    }
}
