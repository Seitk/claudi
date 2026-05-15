//! Watches ~/.claude/projects/**/*.jsonl for transcript updates and folds
//! token/cost data into SessionState.usage.
//!
//! Each line is one event; we care about assistant turns that carry
//! { "type": "assistant", "message": { "usage": {...}, "model": "..." } }.

use crate::cost::cost_usd_for;
use crate::state::{DaemonState, Usage};
use anyhow::Result;
use serde::Deserialize;
use serde_json::Value;
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Duration;
use tokio::io::{AsyncBufReadExt, AsyncSeekExt, BufReader};
use tokio::sync::Mutex;

#[derive(Default)]
struct FileCursor {
    pos: u64,
    /// fingerprint to detect rotation (Claude Code recreates transcripts rarely, but be safe)
    inode: u64,
}

pub async fn run(state: Arc<Mutex<DaemonState>>) -> Result<()> {
    let root = match dirs::home_dir() {
        Some(h) => h.join(".claude").join("projects"),
        None => return Ok(()),
    };
    if !root.exists() {
        tracing::info!(?root, "claude projects dir doesn't exist yet — tailer idle");
    }
    let mut cursors: HashMap<PathBuf, FileCursor> = HashMap::new();

    loop {
        if let Err(e) = scan_once(&root, &mut cursors, &state).await {
            tracing::debug!("tailer scan error: {e}");
        }
        tokio::time::sleep(Duration::from_secs(1)).await;
    }
}

async fn scan_once(
    root: &Path,
    cursors: &mut HashMap<PathBuf, FileCursor>,
    state: &Arc<Mutex<DaemonState>>,
) -> Result<()> {
    if !root.exists() {
        return Ok(());
    }
    for project in std::fs::read_dir(root)? {
        let project = project?;
        if !project.file_type()?.is_dir() {
            continue;
        }
        for entry in std::fs::read_dir(project.path())? {
            let entry = entry?;
            let p = entry.path();
            if p.extension().and_then(|s| s.to_str()) != Some("jsonl") {
                continue;
            }
            tail_one(&p, cursors, state).await?;
        }
    }
    Ok(())
}

async fn tail_one(
    path: &Path,
    cursors: &mut HashMap<PathBuf, FileCursor>,
    state: &Arc<Mutex<DaemonState>>,
) -> Result<()> {
    let meta = match tokio::fs::metadata(path).await {
        Ok(m) => m,
        Err(_) => return Ok(()),
    };
    // Skip transcripts that haven't been modified in the last hour — daemon focuses on live sessions.
    if let Ok(mtime) = meta.modified() {
        if let Ok(age) = std::time::SystemTime::now().duration_since(mtime) {
            if age > Duration::from_secs(60 * 60) {
                return Ok(());
            }
        }
    }
    let inode = inode_of(&meta);
    let size = meta.len();

    let cursor = cursors.entry(path.to_path_buf()).or_default();
    if cursor.inode != inode {
        cursor.inode = inode;
        cursor.pos = 0;
    }
    if size <= cursor.pos {
        return Ok(());
    }

    let mut f = tokio::fs::File::open(path).await?;
    f.seek(std::io::SeekFrom::Start(cursor.pos)).await?;
    let reader = BufReader::new(f);
    let mut lines = reader.lines();
    let mut new_pos = cursor.pos;
    while let Some(line) = lines.next_line().await? {
        new_pos += line.len() as u64 + 1; // +newline
        if line.trim().is_empty() {
            continue;
        }
        if let Err(e) = process_line(&line, state).await {
            tracing::debug!("tailer line parse: {e}");
        }
    }
    cursor.pos = new_pos;
    Ok(())
}

#[cfg(unix)]
fn inode_of(meta: &std::fs::Metadata) -> u64 {
    use std::os::unix::fs::MetadataExt;
    meta.ino()
}

#[cfg(not(unix))]
fn inode_of(_: &std::fs::Metadata) -> u64 {
    0
}

#[derive(Deserialize)]
struct TranscriptEvent {
    #[serde(rename = "type")]
    kind: Option<String>,
    #[serde(rename = "sessionId")]
    session_id: Option<String>,
    cwd: Option<String>,
    message: Option<TranscriptMessage>,
}

#[derive(Deserialize)]
struct TranscriptMessage {
    model: Option<String>,
    usage: Option<TranscriptUsage>,
}

#[derive(Deserialize, Default)]
struct TranscriptUsage {
    #[serde(default)]
    input_tokens: u64,
    #[serde(default)]
    output_tokens: u64,
    #[serde(default)]
    cache_creation_input_tokens: u64,
    #[serde(default)]
    cache_read_input_tokens: u64,
}

async fn process_line(line: &str, state: &Arc<Mutex<DaemonState>>) -> Result<()> {
    let v: Value = serde_json::from_str(line)?;
    let ev: TranscriptEvent = serde_json::from_value(v)?;
    let id = match ev.session_id {
        Some(s) => s,
        None => return Ok(()),
    };
    let cwd = ev.cwd.map(PathBuf::from).unwrap_or_default();

    let mut s = state.lock().await;
    let sess = s.upsert_session(&id, cwd);

    if ev.kind.as_deref() == Some("assistant") {
        if let Some(msg) = ev.message {
            if let Some(model) = msg.model {
                sess.model = Some(model.clone());
                if let Some(u) = msg.usage {
                    fold_usage(&mut sess.usage, &model, &u);
                }
            }
        }
    }
    s.mark_dirty();
    Ok(())
}

fn fold_usage(usage: &mut Usage, model: &str, u: &TranscriptUsage) {
    usage.input_tokens += u.input_tokens;
    usage.output_tokens += u.output_tokens;
    usage.cache_creation_tokens += u.cache_creation_input_tokens;
    usage.cache_read_tokens += u.cache_read_input_tokens;
    let cost = cost_usd_for(model, u.input_tokens, u.output_tokens, u.cache_creation_input_tokens, u.cache_read_input_tokens);
    usage.cost_usd += cost;
    *usage.by_model.entry(model.to_string()).or_insert(0) += u.input_tokens + u.output_tokens;
}
