use std::fs;
use std::path::{Path, PathBuf};

use anyhow::{anyhow, Context, Result};
use chrono::{DateTime, Utc};
use directories::ProjectDirs;
use niconeon_domain::{CommentEvent, RegexFilter};
use rusqlite::{params, Connection, OptionalExtension};

pub struct Store {
    data_conn: Connection,
    cache_conn: Connection,
}

impl Store {
    pub fn open_default() -> Result<Self> {
        let data_path = db_path().ok_or_else(|| anyhow!("failed to determine data directory"))?;
        let cache_path =
            cache_db_path().ok_or_else(|| anyhow!("failed to determine cache directory"))?;
        Self::open_with_paths(data_path, cache_path)
    }

    pub fn open_memory() -> Result<Self> {
        let data_conn = Connection::open_in_memory().context("open sqlite data in-memory")?;
        let cache_conn = Connection::open_in_memory().context("open sqlite cache in-memory")?;
        let store = Self {
            data_conn,
            cache_conn,
        };
        store.init_data_schema()?;
        store.init_cache_schema()?;
        Ok(store)
    }

    pub fn open_with_path(path: PathBuf) -> Result<Self> {
        let cache_path = path.with_file_name("niconeon-cache.db");
        Self::open_with_paths(path, cache_path)
    }

    pub fn open_with_paths(data_path: PathBuf, cache_path: PathBuf) -> Result<Self> {
        for path in [&data_path, &cache_path] {
            if let Some(parent) = path.parent() {
                fs::create_dir_all(parent)
                    .with_context(|| format!("create dir {}", parent.display()))?;
            }
        }

        let data_conn = Connection::open(&data_path)
            .with_context(|| format!("open sqlite {}", data_path.display()))?;
        data_conn
            .pragma_update(None, "journal_mode", "WAL")
            .context("set WAL mode for data db")?;

        let cache_conn = Connection::open(&cache_path)
            .with_context(|| format!("open sqlite {}", cache_path.display()))?;
        cache_conn
            .pragma_update(None, "journal_mode", "WAL")
            .context("set WAL mode for cache db")?;

        let store = Self {
            data_conn,
            cache_conn,
        };
        store.init_data_schema()?;
        store.init_cache_schema()?;
        store.migrate_comment_cache_if_needed()?;
        Ok(store)
    }

    fn init_data_schema(&self) -> Result<()> {
        self.data_conn.execute_batch(
            r#"
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

    fn init_cache_schema(&self) -> Result<()> {
        self.cache_conn.execute_batch(
            r#"
            CREATE TABLE IF NOT EXISTS comment_cache (
                video_id TEXT PRIMARY KEY,
                fetched_at TEXT NOT NULL,
                payload_json TEXT NOT NULL
            );
            "#,
        )?;
        Ok(())
    }

    fn migrate_comment_cache_if_needed(&self) -> Result<()> {
        if !table_exists(&self.data_conn, "comment_cache")? {
            return Ok(());
        }

        let mut stmt = self
            .data_conn
            .prepare("SELECT video_id, fetched_at, payload_json FROM comment_cache")
            .context("prepare legacy comment cache select")?;
        let rows = stmt.query_map([], |row| {
            Ok((
                row.get::<_, String>(0)?,
                row.get::<_, String>(1)?,
                row.get::<_, String>(2)?,
            ))
        })?;

        for row in rows {
            let (video_id, fetched_at, payload_json) = row?;
            self.cache_conn.execute(
                r#"
                INSERT INTO comment_cache(video_id, fetched_at, payload_json)
                VALUES (?1, ?2, ?3)
                ON CONFLICT(video_id)
                DO UPDATE SET fetched_at = excluded.fetched_at, payload_json = excluded.payload_json
                "#,
                params![video_id, fetched_at, payload_json],
            )?;
        }

        self.data_conn
            .execute("DROP TABLE IF EXISTS comment_cache", [])
            .context("drop legacy comment_cache table")?;

        Ok(())
    }

    pub fn load_comment_cache(&self, video_id: &str) -> Result<Option<Vec<CommentEvent>>> {
        let payload: Option<String> = self
            .cache_conn
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
        self.cache_conn.execute(
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
        self.data_conn.execute(
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
        let changed = self.data_conn.execute(
            "INSERT OR IGNORE INTO ng_users(user_id, created_at) VALUES (?1, ?2)",
            params![user_id, now],
        )?;
        Ok(changed > 0)
    }

    pub fn remove_ng_user(&self, user_id: &str) -> Result<bool> {
        let changed = self
            .data_conn
            .execute("DELETE FROM ng_users WHERE user_id = ?1", params![user_id])?;
        Ok(changed > 0)
    }

    pub fn list_ng_users(&self) -> Result<Vec<String>> {
        let mut stmt = self
            .data_conn
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
        self.data_conn.execute(
            "INSERT INTO regex_filters(pattern, created_at) VALUES (?1, ?2)",
            params![pattern, now.to_rfc3339()],
        )?;
        let id = self.data_conn.last_insert_rowid();

        Ok(RegexFilter {
            filter_id: id,
            pattern: pattern.to_string(),
            created_at: now,
        })
    }

    pub fn remove_regex_filter(&self, filter_id: i64) -> Result<bool> {
        let changed = self.data_conn.execute(
            "DELETE FROM regex_filters WHERE id = ?1",
            params![filter_id],
        )?;
        Ok(changed > 0)
    }

    pub fn list_regex_filters(&self) -> Result<Vec<RegexFilter>> {
        let mut stmt = self
            .data_conn
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

pub fn cache_db_path() -> Option<PathBuf> {
    let dirs = ProjectDirs::from("com", "sh4869221b", "niconeon")?;
    Some(dirs.cache_dir().join("comment-cache.db"))
}

fn table_exists(conn: &Connection, table_name: &str) -> Result<bool> {
    let found = conn
        .query_row(
            "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?1 LIMIT 1",
            params![table_name],
            |_| Ok(()),
        )
        .optional()?
        .is_some();
    Ok(found)
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
    use std::fs;
    use std::path::Path;
    use std::time::{SystemTime, UNIX_EPOCH};

    use niconeon_domain::CommentEvent;
    use rusqlite::{params, Connection, OptionalExtension};

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
        assert_eq!(
            extract_video_id_from_path(Path::new("/tmp/no-id.mp4")),
            None
        );
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

    #[test]
    fn migrates_legacy_comment_cache_table() {
        let ts = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("time")
            .as_nanos();
        let base_dir = std::env::temp_dir().join(format!("niconeon-store-test-{ts}"));
        fs::create_dir_all(&base_dir).expect("create temp dir");

        let data_path = base_dir.join("niconeon.db");
        let cache_path = base_dir.join("comment-cache.db");

        let legacy_conn = Connection::open(&data_path).expect("open legacy db");
        legacy_conn
            .execute_batch(
                r#"
                CREATE TABLE comment_cache (
                    video_id TEXT PRIMARY KEY,
                    fetched_at TEXT NOT NULL,
                    payload_json TEXT NOT NULL
                );
                "#,
            )
            .expect("create legacy table");
        legacy_conn
            .execute(
                "INSERT INTO comment_cache(video_id, fetched_at, payload_json) VALUES (?1, ?2, ?3)",
                params![
                    "sm9",
                    "2024-01-01T00:00:00Z",
                    "[{\"comment_id\":\"c1\",\"at_ms\":1,\"user_id\":\"u1\",\"text\":\"legacy\"}]"
                ],
            )
            .expect("insert legacy row");
        drop(legacy_conn);

        let store =
            Store::open_with_paths(data_path.clone(), cache_path.clone()).expect("open store");
        let loaded = store
            .load_comment_cache("sm9")
            .expect("load migrated cache")
            .expect("cache exists");
        assert_eq!(loaded.len(), 1);
        assert_eq!(loaded[0].text, "legacy");

        let data_conn = Connection::open(&data_path).expect("reopen data db");
        let legacy_exists: bool = data_conn
            .query_row(
                "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'comment_cache' LIMIT 1",
                [],
                |_| Ok(()),
            )
            .optional()
            .expect("query legacy table")
            .is_some();
        assert!(!legacy_exists);
    }
}
