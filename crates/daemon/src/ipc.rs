//! Unix-socket IPC: hook CLI and `claudi` CLI talk to the daemon here.
//!
//! Wire format: length-prefixed JSON.
//!     [u32 LE length][json bytes]
//!
//! Each request gets one response.

use crate::state::{ApprovalDecision, DaemonState, FeedItem, PendingApproval, Status};
use anyhow::Result;
use serde::{Deserialize, Serialize};
use std::path::PathBuf;
use std::sync::Arc;
use std::time::{Duration, Instant, SystemTime};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{UnixListener, UnixStream};
use tokio::sync::Mutex;

pub fn socket_path() -> PathBuf {
    PathBuf::from("/tmp/claudi.sock")
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum Request {
    /// `claudi status` — debug info
    Status,
    /// Generic hook event payload from Claude Code
    Hook {
        event: String,
        #[serde(default)]
        session_id: Option<String>,
        #[serde(default)]
        cwd: Option<String>,
        #[serde(default)]
        transcript_path: Option<String>,
        /// Raw hook payload, untyped — we extract fields per event type
        #[serde(default)]
        payload: serde_json::Value,
    },
    /// Block on touch approve/deny for a PreToolUse intercept
    AwaitApproval {
        session_id: String,
        tool: String,
        command: String,
        timeout_ms: u64,
    },
    /// Render the current frame and return a PNG (base64) — debug only.
    RenderPng,
    /// Force the daemon to switch to a given screen.
    SetScreen { name: String },
    /// Focus a session by id (e.g. for the Feed screen).
    Focus { session_id: String },
    /// Ask the daemon to forward a Reboot message to the device.
    Reboot { mode: u8 },
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum Response {
    Ok,
    Status {
        sessions: usize,
        device_connected: bool,
        backlight: u8,
    },
    Approval {
        decision: String, // "approve" | "deny" | "timeout"
    },
    Png {
        base64: String,
    },
    Error {
        message: String,
    },
}

pub async fn serve(state: Arc<Mutex<DaemonState>>) -> Result<()> {
    let path = socket_path();
    let _ = std::fs::remove_file(&path);
    let listener = UnixListener::bind(&path)?;
    tracing::info!(?path, "ipc listening");

    loop {
        let (stream, _) = listener.accept().await?;
        let state = state.clone();
        tokio::spawn(async move {
            if let Err(e) = handle(stream, state).await {
                tracing::warn!("ipc client error: {e}");
            }
        });
    }
}

async fn handle(mut stream: UnixStream, state: Arc<Mutex<DaemonState>>) -> Result<()> {
    let mut len_buf = [0u8; 4];
    stream.read_exact(&mut len_buf).await?;
    let len = u32::from_le_bytes(len_buf) as usize;
    if len > 4 * 1024 * 1024 {
        anyhow::bail!("oversized IPC request: {len}");
    }
    let mut payload = vec![0u8; len];
    stream.read_exact(&mut payload).await?;

    let req: Request = serde_json::from_slice(&payload)?;
    let resp = process(&state, req).await;
    let body = serde_json::to_vec(&resp)?;
    stream.write_all(&(body.len() as u32).to_le_bytes()).await?;
    stream.write_all(&body).await?;
    stream.flush().await?;
    Ok(())
}

async fn process(state: &Arc<Mutex<DaemonState>>, req: Request) -> Response {
    match req {
        Request::Status => {
            let s = state.lock().await;
            Response::Status {
                sessions: s.sessions.len(),
                device_connected: s.device_connected,
                backlight: s.backlight,
            }
        }
        Request::Hook { event, session_id, cwd, transcript_path, payload } => {
            crate::hooks::dispatch(state, &event, session_id, cwd, transcript_path, payload).await;
            Response::Ok
        }
        Request::AwaitApproval { session_id, tool, command, timeout_ms } => {
            let (tx, mut rx) = tokio::sync::mpsc::channel(1);
            {
                let mut s = state.lock().await;
                s.pending_approval = Some(PendingApproval {
                    session_id: session_id.clone(),
                    tool: tool.clone(),
                    command_or_args: command.clone(),
                    created_at: Instant::now(),
                    timeout: Duration::from_millis(timeout_ms),
                    reply_tx: tx,
                });
                // also drop a feed item so we can see it later
                if let Some(sess) = s.sessions.get_mut(&session_id) {
                    sess.push_feed(FeedItem {
                        at: SystemTime::now(),
                        tool: tool.clone(),
                        summary: format!("approval requested: {command}"),
                    });
                    sess.status = Status::Waiting;
                }
                s.screen = crate::state::Screen::Approval;
                s.mark_dirty();
            }

            let decision = match tokio::time::timeout(
                Duration::from_millis(timeout_ms),
                rx.recv(),
            )
            .await
            {
                Ok(Some(d)) => d,
                Ok(None) => ApprovalDecision::Timeout,
                Err(_) => ApprovalDecision::Timeout,
            };

            {
                let mut s = state.lock().await;
                s.pending_approval = None;
                s.screen = crate::state::Screen::Sessions;
                s.mark_dirty();
            }

            Response::Approval {
                decision: match decision {
                    ApprovalDecision::Approve => "approve".into(),
                    ApprovalDecision::Deny => "deny".into(),
                    ApprovalDecision::Timeout => "timeout".into(),
                },
            }
        }
        Request::RenderPng => {
            let s = state.lock().await;
            let png = crate::render::render_png_snapshot(&s);
            use base64::Engine;
            Response::Png { base64: base64::engine::general_purpose::STANDARD.encode(png) }
        }
        Request::Reboot { mode } => {
            let mut s = state.lock().await;
            s.pending_device_command = Some(crate::state::DeviceCommand::Reboot { mode });
            Response::Ok
        }
        Request::Focus { session_id } => {
            let mut s = state.lock().await;
            if s.sessions.contains_key(&session_id) {
                s.focused = Some(session_id);
                s.mark_dirty();
            }
            Response::Ok
        }
        Request::SetScreen { name } => {
            let mut s = state.lock().await;
            let next = match name.as_str() {
                "sessions" => Some(crate::state::Screen::Sessions),
                "feed" => Some(crate::state::Screen::Feed),
                "metrics" => Some(crate::state::Screen::Metrics),
                "approval" => {
                    if s.pending_approval.is_none() {
                        let (tx, _rx) = tokio::sync::mpsc::channel(1);
                        s.pending_approval = Some(PendingApproval {
                            session_id: "demo-approval-session".into(),
                            tool: "Bash".into(),
                            command_or_args: "rm -rf node_modules && pnpm install".into(),
                            created_at: Instant::now(),
                            timeout: Duration::from_secs(45),
                            reply_tx: tx,
                        });
                    }
                    Some(crate::state::Screen::Approval)
                }
                _ => None,
            };
            if let Some(sc) = next {
                s.screen = sc;
                s.mark_dirty();
            }
            Response::Ok
        }
    }
}

/// Helper used by `claudi` CLI to talk to the daemon.
pub async fn call(req: &Request) -> Result<Response> {
    let mut stream = UnixStream::connect(socket_path()).await?;
    let body = serde_json::to_vec(req)?;
    stream.write_all(&(body.len() as u32).to_le_bytes()).await?;
    stream.write_all(&body).await?;
    stream.flush().await?;
    let mut len_buf = [0u8; 4];
    stream.read_exact(&mut len_buf).await?;
    let len = u32::from_le_bytes(len_buf) as usize;
    let mut payload = vec![0u8; len];
    stream.read_exact(&mut payload).await?;
    Ok(serde_json::from_slice(&payload)?)
}

#[allow(dead_code)]
pub fn cwd_to_pathbuf(opt: Option<String>) -> PathBuf {
    opt.map(PathBuf::from).unwrap_or_else(|| std::env::current_dir().unwrap_or_default())
}
