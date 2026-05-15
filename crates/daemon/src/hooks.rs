//! Handlers for incoming hook events. Mutates DaemonState.

use crate::state::{DaemonState, FeedItem, Status};
use serde_json::Value;
use std::path::PathBuf;
use std::sync::Arc;
use std::time::SystemTime;
use tokio::sync::Mutex;

pub async fn dispatch(
    state: &Arc<Mutex<DaemonState>>,
    event: &str,
    session_id: Option<String>,
    cwd: Option<String>,
    transcript_path: Option<String>,
    payload: Value,
) {
    let id = match session_id {
        Some(s) if !s.is_empty() => s,
        _ => return, // can't track without a session id
    };
    let cwd = cwd.map(PathBuf::from).unwrap_or_default();

    let mut s = state.lock().await;
    let sess = s.upsert_session(&id, cwd);
    if let Some(tp) = transcript_path {
        sess.transcript_path = Some(PathBuf::from(tp));
    }

    match event {
        "session_start" | "SessionStart" => {
            sess.status = Status::Idle;
            sess.push_feed(FeedItem {
                at: SystemTime::now(),
                tool: "session".into(),
                summary: "session started".into(),
            });
        }
        "user_prompt_submit" | "UserPromptSubmit" => {
            let prompt = payload.get("prompt").and_then(|v| v.as_str()).unwrap_or("");
            let preview = truncate(prompt.trim(), 60);
            sess.status = Status::Working { tool: "thinking".into() };
            sess.push_feed(FeedItem {
                at: SystemTime::now(),
                tool: "user".into(),
                summary: format!("> {preview}"),
            });
        }
        "pre_tool_use" | "PreToolUse" => {
            let tool = payload.get("tool_name").and_then(|v| v.as_str()).unwrap_or("tool");
            let summary = describe_tool(tool, payload.get("tool_input"));
            sess.status = Status::Working { tool: tool.into() };
            sess.push_feed(FeedItem {
                at: SystemTime::now(),
                tool: tool.into(),
                summary,
            });
        }
        "post_tool_use" | "PostToolUse" => {
            // returning to thinking; don't push a feed line — the pre_tool_use one already covered it
            sess.status = Status::Working { tool: "thinking".into() };
        }
        "notification" | "Notification" => {
            let msg = payload.get("message").and_then(|v| v.as_str()).unwrap_or("");
            sess.status = Status::Waiting;
            sess.push_feed(FeedItem {
                at: SystemTime::now(),
                tool: "notify".into(),
                summary: truncate(msg, 80),
            });
        }
        "stop" | "Stop" => {
            sess.status = Status::Idle;
            sess.push_feed(FeedItem {
                at: SystemTime::now(),
                tool: "stop".into(),
                summary: "turn complete".into(),
            });
            // queue a beep on the device — caller (link) reads this via DaemonState
            // (a real impl would push to a beep queue; v1 just flips status)
        }
        "session_end" | "SessionEnd" => {
            sess.status = Status::Stopped;
            sess.push_feed(FeedItem {
                at: SystemTime::now(),
                tool: "session".into(),
                summary: "session ended".into(),
            });
        }
        other => {
            tracing::debug!("unhandled hook event: {other}");
        }
    }
    s.mark_dirty();
}

fn truncate(s: &str, n: usize) -> String {
    if s.chars().count() <= n {
        s.into()
    } else {
        let mut out: String = s.chars().take(n.saturating_sub(1)).collect();
        out.push('…');
        out
    }
}

fn describe_tool(name: &str, input: Option<&Value>) -> String {
    let one = |key: &str| {
        input
            .and_then(|v| v.get(key))
            .and_then(|v| v.as_str())
            .map(|s| s.to_string())
    };
    match name {
        "Bash" => one("command").map(|c| format!("$ {}", truncate(&c, 60))).unwrap_or_else(|| "Bash".into()),
        "Read" => one("file_path").map(|p| format!("read {}", short_path(&p))).unwrap_or_else(|| "Read".into()),
        "Edit" => one("file_path").map(|p| format!("edit {}", short_path(&p))).unwrap_or_else(|| "Edit".into()),
        "Write" => one("file_path").map(|p| format!("write {}", short_path(&p))).unwrap_or_else(|| "Write".into()),
        "Grep" => one("pattern").map(|p| format!("grep \"{}\"", truncate(&p, 40))).unwrap_or_else(|| "Grep".into()),
        "Glob" => one("pattern").map(|p| format!("glob {}", truncate(&p, 50))).unwrap_or_else(|| "Glob".into()),
        "WebFetch" => one("url").map(|u| format!("fetch {}", truncate(&u, 50))).unwrap_or_else(|| "WebFetch".into()),
        "Agent" => one("description").map(|d| format!("agent: {}", truncate(&d, 50))).unwrap_or_else(|| "Agent".into()),
        other => other.to_string(),
    }
}

fn short_path(p: &str) -> String {
    // strip $HOME prefix
    if let Some(home) = dirs::home_dir() {
        if let Some(rest) = p.strip_prefix(&*home.to_string_lossy()) {
            return format!("~{}", rest);
        }
    }
    // keep last 3 components
    let parts: Vec<&str> = p.split('/').filter(|s| !s.is_empty()).collect();
    if parts.len() > 3 {
        format!(".../{}", parts[parts.len() - 3..].join("/"))
    } else {
        p.to_string()
    }
}
