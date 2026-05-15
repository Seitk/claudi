//! Device link task: connect to the device, push frames, receive touch.
//!
//! Two transports are supported:
//!   - Real USB CDC: macOS exposes /dev/cu.usbmodem*, opened via `serialport`.
//!   - PTY (simulator): a /dev/ttys* path opened as a raw fd; no termios fiddling.
//!
//! Override the path with CLAUDI_DEVICE. Otherwise we auto-discover usbmodem*.

use crate::render::Renderer;
use crate::state::{ApprovalDecision, DaemonState, Screen};
use anyhow::Result;
use claudi_protocol::{Message, TouchPhase, DISPLAY_HEIGHT, DISPLAY_WIDTH, PROTOCOL_VERSION};
use std::io::{Read, Write};
use std::sync::Arc;
use std::time::{Duration, Instant};
use tokio::sync::Mutex;

pub fn device_path_env() -> Option<String> {
    std::env::var("CLAUDI_DEVICE").ok().filter(|s| !s.is_empty())
}

pub async fn run(state: Arc<Mutex<DaemonState>>) -> Result<()> {
    loop {
        let r = connect_and_serve(state.clone()).await;
        {
            let mut s = state.lock().await;
            s.device_connected = false;
            s.mark_dirty();
        }
        if let Err(e) = r {
            tracing::warn!("link disconnected: {e}");
        }
        tokio::time::sleep(Duration::from_millis(500)).await;
    }
}

/// Trait object for the two transports.
trait LinkIo: Read + Write + Send {}
impl<T: Read + Write + Send + ?Sized> LinkIo for T {}

fn open_link(path: &str) -> Result<Box<dyn LinkIo>> {
    // Treat anything other than /dev/cu.* and /dev/tty.* as a pty/regular fd.
    let is_pty = !(path.starts_with("/dev/cu.") || path.starts_with("/dev/tty.usbmodem") || path.starts_with("/dev/tty.usbserial"));
    if is_pty {
        use std::os::fd::FromRawFd;
        let path_c = std::ffi::CString::new(path).map_err(|e| anyhow::anyhow!(e))?;
        let fd = unsafe { libc::open(path_c.as_ptr(), libc::O_RDWR | libc::O_NOCTTY) };
        if fd < 0 {
            return Err(std::io::Error::last_os_error().into());
        }
        // Raw mode — no line discipline, no \r/\n translation.
        unsafe {
            let mut tio: libc::termios = std::mem::zeroed();
            if libc::tcgetattr(fd, &mut tio) == 0 {
                libc::cfmakeraw(&mut tio);
                libc::tcsetattr(fd, libc::TCSANOW, &tio);
            }
            let flags = libc::fcntl(fd, libc::F_GETFL);
            libc::fcntl(fd, libc::F_SETFL, flags | libc::O_NONBLOCK);
        }
        Ok(Box::new(unsafe { std::fs::File::from_raw_fd(fd) }))
    } else {
        let port = serialport::new(path, 1_000_000)
            .timeout(Duration::from_millis(20))
            .open_native()?;
        Ok(Box::new(port))
    }
}

async fn connect_and_serve(state: Arc<Mutex<DaemonState>>) -> Result<()> {
    let path = match device_path_env() {
        Some(p) => p,
        None => match find_device_port() {
            Some(p) => p,
            None => {
                tokio::time::sleep(Duration::from_millis(1500)).await;
                anyhow::bail!("no candidate USB CDC port found");
            }
        },
    };
    tracing::info!("opening device at {}", path);
    let mut port = open_link(&path)?;
    {
        let mut s = state.lock().await;
        s.device_connected = true;
        s.mark_dirty();
    }

    write_msg(&mut *port, &Message::Hello {
        version: PROTOCOL_VERSION,
        width: DISPLAY_WIDTH,
        height: DISPLAY_HEIGHT,
    })?;
    let now_secs = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    write_msg(&mut *port, &Message::SetTime { unix_seconds: now_secs })?;

    let mut renderer = Renderer::new();
    let mut last_dirty: u64 = u64::MAX;
    let mut full_repaint_pending = true;
    let mut rx_buf: Vec<u8> = Vec::with_capacity(4096);
    let mut ping_seq: u32 = 0;
    let mut last_ping = Instant::now();
    let mut last_rx = Instant::now();

    let frame_period = Duration::from_millis(66); // ~15 fps
    let mut next_tick = Instant::now() + frame_period;
    let mut frame_id: u32 = 0;

    loop {
        let mut tmp = [0u8; 1024];
        match port.read(&mut tmp) {
            Ok(0) => {
                // For pty this means EOF; for serial we shouldn't see 0 except on close.
                tokio::time::sleep(Duration::from_millis(20)).await;
            }
            Ok(n) => {
                rx_buf.extend_from_slice(&tmp[..n]);
                last_rx = Instant::now();
            }
            Err(e)
                if matches!(
                    e.kind(),
                    std::io::ErrorKind::TimedOut | std::io::ErrorKind::WouldBlock
                ) => {}
            Err(e) => return Err(e.into()),
        }
        while let Some((msg, consumed)) = Message::decode(&rx_buf)? {
            handle_inbound(&state, msg).await;
            rx_buf.drain(..consumed);
        }

        if Instant::now() >= next_tick {
            next_tick += frame_period;
            let (current_dirty_seq, want_full) = {
                let s = state.lock().await;
                (s.dirty_seq, full_repaint_pending)
            };
            if want_full || current_dirty_seq != last_dirty {
                let s = state.lock().await;
                let frame = renderer.render_frame(&s);
                drop(s);
                frame_id = frame_id.wrapping_add(1);
                write_msg(&mut *port, &Message::FrameBegin { frame_id })?;
                for rect in frame.rects {
                    write_msg(&mut *port, &Message::Rect {
                        x: rect.x, y: rect.y, w: rect.w, h: rect.h, pixels: rect.pixels,
                    })?;
                }
                write_msg(&mut *port, &Message::FrameEnd)?;
                last_dirty = current_dirty_seq;
                full_repaint_pending = false;
            }
        }

        // Forward any pending device command (reboot, etc.)
        let cmd = {
            let mut s = state.lock().await;
            s.pending_device_command.take()
        };
        if let Some(c) = cmd {
            match c {
                crate::state::DeviceCommand::Reboot { mode } => {
                    write_msg(&mut *port, &Message::Reboot { mode })?;
                    // The device is about to drop USB; bail out so the reconnect
                    // loop opens it again under its new identity.
                    return Ok(());
                }
            }
        }

        if last_ping.elapsed() > Duration::from_secs(2) {
            ping_seq = ping_seq.wrapping_add(1);
            write_msg(&mut *port, &Message::Ping { seq: ping_seq })?;
            last_ping = Instant::now();
        }

        if last_rx.elapsed() > Duration::from_secs(8) {
            anyhow::bail!("no inbound from device for 8s");
        }

        tokio::time::sleep(Duration::from_millis(10)).await;
    }
}

/// Writes the full message, retrying on EAGAIN/WouldBlock so a busy PTY/serial
/// buffer doesn't tear the link.
fn write_msg<W: Write + ?Sized>(w: &mut W, msg: &Message) -> std::io::Result<()> {
    let mut buf = Vec::with_capacity(64);
    msg.encode(&mut buf).map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e.to_string()))?;
    let mut written = 0;
    let mut retries = 0u32;
    while written < buf.len() {
        match w.write(&buf[written..]) {
            Ok(0) => return Err(std::io::Error::new(std::io::ErrorKind::WriteZero, "write returned 0")),
            Ok(n) => {
                written += n;
                retries = 0;
            }
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                retries += 1;
                if retries > 2000 {
                    return Err(std::io::Error::new(std::io::ErrorKind::TimedOut, "write blocked too long"));
                }
                std::thread::sleep(std::time::Duration::from_micros(500));
            }
            Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
            Err(e) => return Err(e),
        }
    }
    Ok(())
}

fn find_device_port() -> Option<String> {
    let ports = serialport::available_ports().ok()?;
    for p in ports {
        if p.port_name.contains("usbmodem") || p.port_name.contains("usbserial") {
            return Some(p.port_name);
        }
    }
    None
}

async fn handle_inbound(state: &Arc<Mutex<DaemonState>>, msg: Message) {
    match msg {
        Message::HelloAck { fw_version, .. } => {
            tracing::info!("device handshake, fw v{fw_version}");
        }
        Message::Touch { phase: TouchPhase::Up, x, y, .. } => {
            handle_tap(state, x, y).await;
        }
        Message::Gesture { kind, .. } => {
            handle_gesture(state, kind).await;
        }
        Message::Pong { .. } => {}
        Message::Log { text } => tracing::debug!("fw: {text}"),
        _ => {}
    }
}

async fn handle_tap(state: &Arc<Mutex<DaemonState>>, x: u16, y: u16) {
    let mut s = state.lock().await;
    if matches!(s.screen, Screen::Approval) {
        if let Some(pa) = s.pending_approval.as_ref() {
            // bottom button row spans roughly the bottom 100px
            if y > DISPLAY_HEIGHT - 110 {
                let decision = if x < DISPLAY_WIDTH / 2 { ApprovalDecision::Deny } else { ApprovalDecision::Approve };
                let tx = pa.reply_tx.clone();
                drop(s);
                let _ = tx.send(decision).await;
            }
        }
        return;
    }
    if matches!(s.screen, Screen::Sessions) {
        let inner_y = y as i32 - crate::render::STATUS_BAR_HEIGHT as i32 - 8;
        if inner_y < 0 { return; }
        let row = (inner_y / crate::render::SESSION_ROW_HEIGHT as i32) as usize;
        // sort same way the renderer does so the row index matches
        let mut sessions: Vec<&crate::state::SessionState> = s.sessions.values().collect();
        sessions.sort_by(|a, b| b.last_activity.cmp(&a.last_activity));
        if let Some(target) = sessions.get(row) {
            let id = target.id.clone();
            s.focused = Some(id);
            s.screen = Screen::Feed;
            s.mark_dirty();
        }
    }
}

async fn handle_gesture(state: &Arc<Mutex<DaemonState>>, kind: claudi_protocol::GestureKind) {
    use claudi_protocol::GestureKind::*;
    let mut s = state.lock().await;
    let order = [Screen::Sessions, Screen::Feed, Screen::Metrics];
    let idx = order.iter().position(|sc| *sc == s.screen).unwrap_or(0);
    let next = match kind {
        SwipeLeft => (idx + 1) % order.len(),
        SwipeRight => (idx + order.len() - 1) % order.len(),
        _ => idx,
    };
    if order[next] != s.screen {
        s.screen = order[next];
        s.mark_dirty();
    }
}
