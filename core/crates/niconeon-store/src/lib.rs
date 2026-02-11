use std::fs;
use std::path::{Path, PathBuf};

use anyhow::{anyhow, Context, Result};
use chrono::{DateTime, Utc};
use directories::ProjectDirs;
use niconeon_domain::{CommentEvent, RegexFilter};
use rusqlite::{params, Connection, OptionalExtension};

pub struct Store {
    conn: Connection,
}

impl Store {
    pub fn open_default() -> Result<Self> {
        let path = db_path().ok_or_else(|| anyhow!("failed to determine data directory"))?;
        Self::open_with_path(path)
    }

    pub fn open_memory() -> Result<Self> {
        let conn = Connection::open_in_memory().context("open sqlite in-memory")?;
        let store = Self { conn };
        store.init_schema()?;
        Ok(store)
    }

    pub fn open_with_path(path: PathBuf) -> Result<Self> {
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent).with_context(|| format!("create dir {}", parent.display()))?;
        }

        let conn = Connection::open(&path).with_context(|| format!("open sqlite {}", path.display()))?;
        conn.pragma_update(None, "journal_mode", "WAL")
            .context("set WAL mode")?;

        let store = Self { conn };
        store.init_schema()?;
        Ok(store)
    }

    fn init_schema(&self) -> Result<()> {
        self.conn.execute_batch(
            r#"
            CREATE TABLE IF NOT EXISTS comment_cache (
                video_id TEXT PRIMARY KEY,
                fetched_at TEXT NOT NULL,
                payload_json TEXT NOT NULL
            );

            CREATE TABLE IF NOT EXISTS ng_users (
                user_id TEXT PRIMARY KEY,
                created_at TEXT NOT NULL
            );

            CREATE TABLE IF NOT EXISTS regex_filters (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                pattern TEXT NOT NULL,
                created_at TEXT NOT NULL
            );

            CREATE TABLE IF NOT EXISTS video_map (
                video_path TEXT PRIMARY KEY,
                video_id TEXT NOT NULL,
                last_opened_at TEXT NOT NULL
            );
            "#,
        )?;
        Ok(())
    }

    pub fn load_comment_cache(&self, video_id: &str) -> Result<Option<Vec<CommentEvent>>> {
        let payload: Option<String> = self
            .conn
            .query_row(
                "SELECT payload_json FROM comment_cache WHERE video_id = ?1",
                params![video_id],
                |row| row.get(0),
            )
            .optional()?;

        match payload {
            Some(json) => {
                let comments = serde_json::from_str::<Vec<CommentEvent>>(&json)
                    .with_context(|| format!("decode comment cache for {video_id}"))?;
                Ok(Some(comments))
            }
            None => Ok(None),
        }
    }

    pub fn save_comment_cache(&self, video_id: &str, comments: &[CommentEvent]) -> Result<()> {
        let now = Utc::now().to_rfc3339();
        let payload = serde_json::to_string(comments).context("serialize comment cache")?;
        self.conn.execute(
            r#"
            INSERT INTO comment_cache(video_id, fetched_at, payload_json)
            VALUES (?1, ?2, ?3)
            ON CONFLICT(video_id)
            DO UPDATE SET fetched_at = excluded.fetched_at, payload_json = excluded.payload_json
            "#,
            params![video_id, now, payload],
        )?;
        Ok(())
    }

    pub fn upsert_video_map(&self, video_path: &str, video_id: &str) -> Result<()> {
        let now = Utc::now().to_rfc3339();
        self.conn.execute(
            r#"
            INSERT INTO video_map(video_path, video_id, last_opened_at)
            VALUES (?1, ?2, ?3)
            ON CONFLICT(video_path)
            DO UPDATE SET video_id = excluded.video_id, last_opened_at = excluded.last_opened_at
            "#,
            params![video_path, video_id, now],
        )?;
        Ok(())
    }

    pub fn add_ng_user(&self, user_id: &str) -> Result<bool> {
        let now = Utc::now().to_rfc3339();
        let changed = self.conn.execute(
            "INSERT OR IGNORE INTO ng_users(user_id, created_at) VALUES (?1, ?2)",
            params![user_id, now],
        )?;
        Ok(changed > 0)
    }

    pub fn remove_ng_user(&self, user_id: &str) -> Result<bool> {
        let changed = self
            .conn
            .execute("DELETE FROM ng_users WHERE user_id = ?1", params![user_id])?;
        Ok(changed > 0)
    }

    pub fn list_ng_users(&self) -> Result<Vec<String>> {
        let mut stmt = self
            .conn
            .prepare("SELECT user_id FROM ng_users ORDER BY created_at ASC")?;
        let rows = stmt.query_map([], |row| row.get(0))?;

        let mut users = Vec::new();
        for row in rows {
            users.push(row?);
        }
        Ok(users)
    }

    pub fn insert_regex_filter(&self, pattern: &str) -> Result<RegexFilter> {
        let now = Utc::now();
        self.conn.execute(
            "INSERT INTO regex_filters(pattern, created_at) VALUES (?1, ?2)",
            params![pattern, now.to_rfc3339()],
        )?;
        let id = self.conn.last_insert_rowid();

        Ok(RegexFilter {
            filter_id: id,
            pattern: pattern.to_string(),
            created_at: now,
        })
    }

    pub fn remove_regex_filter(&self, filter_id: i64) -> Result<bool> {
        let changed = self
            .conn
            .execute("DELETE FROM regex_filters WHERE id = ?1", params![filter_id])?;
        Ok(changed > 0)
    }

    pub fn list_regex_filters(&self) -> Result<Vec<RegexFilter>> {
        let mut stmt = self
            .conn
            .prepare("SELECT id, pattern, created_at FROM regex_filters ORDER BY id ASC")?;
        let rows = stmt.query_map([], |row| {
            let created_raw: String = row.get(2)?;
            let created_at = DateTime::parse_from_rfc3339(&created_raw)
                .map(|dt| dt.with_timezone(&Utc))
                .map_err(|e| {
                    rusqlite::Error::FromSqlConversionFailure(
                        2,
                        rusqlite::types::Type::Text,
                        Box::new(e),
                    )
                })?;

            Ok(RegexFilter {
                filter_id: row.get(0)?,
                pattern: row.get(1)?,
                created_at,
            })
        })?;

        let mut filters = Vec::new();
        for row in rows {
            filters.push(row?);
        }
        Ok(filters)
    }
}

pub fn db_path() -> Option<PathBuf> {
    let dirs = ProjectDirs::from("com", "sh4869221b", "niconeon")?;
    Some(dirs.data_dir().join("niconeon.db"))
}

pub fn extract_video_id_from_path(path: &Path) -> Option<String> {
    let file_name = path.file_name()?.to_string_lossy();
    let re = regex::Regex::new(r"(?i)(sm|nm|so)(\d+)").expect("valid regex");
    let captures = re.captures(&file_name)?;
    let prefix = captures.get(1)?.as_str().to_lowercase();
    let digits = captures.get(2)?.as_str();
    Some(format!("{prefix}{digits}"))
}

#[cfg(test)]
mod tests {
    use std::path::Path;

    use niconeon_domain::CommentEvent;

    use super::{extract_video_id_from_path, Store};

    #[test]
    fn extract_video_id() {
        assert_eq!(
            extract_video_id_from_path(Path::new("/tmp/abc_sm9_test.mp4")),
            Some("sm9".to_string())
        );
        assert_eq!(
            extract_video_id_from_path(Path::new("/tmp/SO1234.mkv")),
            Some("so1234".to_string())
        );
        assert_eq!(extract_video_id_from_path(Path::new("/tmp/no-id.mp4")), None);
    }

    #[test]
    fn cache_roundtrip() {
        let store = Store::open_memory().expect("store");
        let comments = vec![CommentEvent {
            comment_id: "c1".to_string(),
            at_ms: 100,
            user_id: "u1".to_string(),
            text: "hello".to_string(),
        }];

        store
            .save_comment_cache("sm9", &comments)
            .expect("save cache");
        let loaded = store
            .load_comment_cache("sm9")
            .expect("load")
            .expect("exists");

        assert_eq!(loaded, comments);
    }
}
