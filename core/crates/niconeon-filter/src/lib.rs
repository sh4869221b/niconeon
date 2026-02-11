use std::collections::BTreeSet;

use niconeon_domain::{CommentEvent, RegexFilter};
use regex::Regex;
use thiserror::Error;

#[derive(Debug, Error)]
pub enum FilterError {
    #[error("invalid regex: {0}")]
    InvalidRegex(String),
}

#[derive(Debug, Clone)]
struct CompiledRegexFilter {
    raw: RegexFilter,
    compiled: Regex,
}

#[derive(Debug, Default, Clone)]
pub struct FilterEngine {
    ng_users: BTreeSet<String>,
    regex_filters: Vec<CompiledRegexFilter>,
}

impl FilterEngine {
    pub fn new(ng_users: Vec<String>, regex_filters: Vec<RegexFilter>) -> Result<Self, FilterError> {
        let mut engine = Self {
            ng_users: ng_users.into_iter().collect(),
            regex_filters: Vec::new(),
        };

        for filter in regex_filters {
            engine.push_regex(filter)?;
        }

        Ok(engine)
    }

    pub fn list_ng_users(&self) -> Vec<String> {
        self.ng_users.iter().cloned().collect()
    }

    pub fn list_regex_filters(&self) -> Vec<RegexFilter> {
        self.regex_filters.iter().map(|f| f.raw.clone()).collect()
    }

    pub fn add_ng_user(&mut self, user_id: &str) -> bool {
        self.ng_users.insert(user_id.to_string())
    }

    pub fn remove_ng_user(&mut self, user_id: &str) -> bool {
        self.ng_users.remove(user_id)
    }

    pub fn add_regex_filter(&mut self, filter: RegexFilter) -> Result<(), FilterError> {
        self.push_regex(filter)
    }

    pub fn remove_regex_filter(&mut self, filter_id: i64) -> bool {
        let before = self.regex_filters.len();
        self.regex_filters.retain(|f| f.raw.filter_id != filter_id);
        before != self.regex_filters.len()
    }

    pub fn should_hide(&self, comment: &CommentEvent) -> bool {
        // Filter order is fixed by requirement: NG user first, then regex.
        if self.ng_users.contains(&comment.user_id) {
            return true;
        }

        self.regex_filters
            .iter()
            .any(|f| f.compiled.is_match(&comment.text))
    }

    fn push_regex(&mut self, filter: RegexFilter) -> Result<(), FilterError> {
        let compiled = Regex::new(&filter.pattern)
            .map_err(|e| FilterError::InvalidRegex(e.to_string()))?;
        self.regex_filters.push(CompiledRegexFilter { raw: filter, compiled });
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use chrono::Utc;
    use niconeon_domain::{CommentEvent, RegexFilter};

    use super::FilterEngine;

    fn comment(user_id: &str, text: &str) -> CommentEvent {
        CommentEvent {
            comment_id: "c1".to_string(),
            at_ms: 100,
            user_id: user_id.to_string(),
            text: text.to_string(),
        }
    }

    #[test]
    fn ng_is_applied_before_regex() {
        let mut engine = FilterEngine::new(vec![], vec![]).expect("engine");
        let filter = RegexFilter {
            filter_id: 1,
            pattern: "hello".to_string(),
            created_at: Utc::now(),
        };
        engine.add_regex_filter(filter).expect("regex");
        engine.add_ng_user("u1");

        let ng_comment = comment("u1", "anything");
        let regex_comment = comment("u2", "hello world");
        let keep_comment = comment("u2", "other");

        assert!(engine.should_hide(&ng_comment));
        assert!(engine.should_hide(&regex_comment));
        assert!(!engine.should_hide(&keep_comment));
    }

    #[test]
    fn invalid_regex_is_rejected() {
        let mut engine = FilterEngine::default();
        let filter = RegexFilter {
            filter_id: 1,
            pattern: "(".to_string(),
            created_at: Utc::now(),
        };
        assert!(engine.add_regex_filter(filter).is_err());
    }
}
