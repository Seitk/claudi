//! Daemon-wide state model.

use std::collections::{HashMap, VecDeque};
use std::path::PathBuf;
use std::time::{Duration, Instant, SystemTime};

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Status {
    Starting,
    Idle,
    Working { tool: String },
    Waiting,
    Stopped,
}

impl Status {
    pub fn label(&self) -> &'static str {
        match self {
            Self::Starting => "starting",
            Self::Idle => "idle",
            Self::Working { .. } => "working",
            Self::Waiting => "waiting",
            Self::Stopped => "stopped",
        }
    }
}

#[derive(Debug, Clone)]
pub struct FeedItem {
    pub at: SystemTime,
    pub tool: String,
    pub summary: String,
}

#[derive(Debug, Clone, Default)]
pub struct Usage {
    pub input_tokens: u64,
    pub output_tokens: u64,
    pub cache_creation_tokens: u64,
    pub cache_read_tokens: u64,
    pub cost_usd: f64,
    /// model id -> input+output tokens
    pub by_model: HashMap<String, u64>,
}

#[derive(Debug, Clone)]
pub struct PendingApproval {
    pub session_id: String,
    pub tool: String,
    pub command_or_args: String,
    pub created_at: Instant,
    pub timeout: Duration,
    pub reply_tx: tokio::sync::mpsc::Sender<ApprovalDecision>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ApprovalDecision {
    Approve,
    Deny,
    Timeout,
}

#[derive(Debug, Clone)]
pub struct SessionState {
    pub id: String,
    pub cwd: PathBuf,
    pub transcript_path: Option<PathBuf>,
    pub started_at: SystemTime,
    pub last_activity: Instant,
    pub status: Status,
    pub feed: VecDeque<FeedItem>,
    pub usage: Usage,
    pub model: Option<String>,
}

impl SessionState {
    pub fn new(id: String, cwd: PathBuf) -> Self {
        Self {
            id,
            cwd,
            transcript_path: None,
            started_at: SystemTime::now(),
            last_activity: Instant::now(),
            status: Status::Starting,
            feed: VecDeque::with_capacity(200),
            usage: Usage::default(),
            model: None,
        }
    }

    pub fn push_feed(&mut self, item: FeedItem) {
        if self.feed.len() >= 200 {
            self.feed.pop_back();
        }
        self.feed.push_front(item);
        self.last_activity = Instant::now();
    }

    pub fn short_name(&self) -> String {
        self.cwd
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_else(|| format!("sess-{}", &self.id[..self.id.len().min(8)]))
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Screen {
    Sessions,
    Feed,
    Approval,
    Metrics,
}

#[derive(Debug)]
pub struct DaemonState {
    pub sessions: HashMap<String, SessionState>,
    /// session id of the session currently viewed on the Feed screen
    pub focused: Option<String>,
    pub screen: Screen,
    pub pending_approval: Option<PendingApproval>,
    /// 0..=255, last value we sent for the AMOLED backlight
    pub backlight: u8,
    pub device_connected: bool,
    /// Bumped every time mutating logic believes the visible UI changed.
    pub dirty_seq: u64,
    /// Out-of-band command for the link task to forward on the next tick.
    pub pending_device_command: Option<DeviceCommand>,
}

#[derive(Debug, Clone, Copy)]
pub enum DeviceCommand {
    Reboot { mode: u8 },
}

impl DaemonState {
    pub fn new() -> Self {
        Self {
            sessions: HashMap::new(),
            focused: None,
            screen: Screen::Sessions,
            pending_approval: None,
            backlight: 220,
            device_connected: false,
            dirty_seq: 0,
            pending_device_command: None,
        }
    }

    pub fn mark_dirty(&mut self) {
        self.dirty_seq = self.dirty_seq.wrapping_add(1);
    }

    pub fn upsert_session(&mut self, id: &str, cwd: PathBuf) -> &mut SessionState {
        let new = !self.sessions.contains_key(id);
        let s = self
            .sessions
            .entry(id.to_string())
            .or_insert_with(|| SessionState::new(id.to_string(), cwd.clone()));
        if new {
            self.dirty_seq = self.dirty_seq.wrapping_add(1);
        }
        s
    }

    pub fn focused_session(&self) -> Option<&SessionState> {
        self.focused.as_ref().and_then(|id| self.sessions.get(id))
    }
}
