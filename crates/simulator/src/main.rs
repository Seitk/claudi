//! Device simulator: presents a 368x448 window that speaks the wire protocol
//! over a pair of pipes to a daemon process.
//!
//! Usage:
//!   1. Run `claudi-simulator` — it prints two FDs / paths
//!   2. In another terminal: `CLAUDI_DEVICE=<path> claudid`
//!
//! But the easier flow: simulator opens a PTY and prints its slave path, set
//! that as CLAUDI_DEVICE for the daemon.

use anyhow::Result;
use claudi_protocol::{Message, TouchPhase, DISPLAY_HEIGHT, DISPLAY_WIDTH};
use minifb::{Key, MouseButton, MouseMode, Scale, Window, WindowOptions};
use std::io::{Read, Write};
use std::time::Instant;

fn main() -> Result<()> {
    tracing_subscriber::fmt::init();

    // Open a PTY pair (POSIX): posix_openpt → grantpt → unlockpt → ptsname.
    let (mut master, slave_path) = open_pty()?;
    println!("simulator pty: {}", slave_path);
    println!("run: CLAUDI_DEVICE={} cargo run -p claudid", slave_path);
    set_nonblocking(&master)?;

    let w = DISPLAY_WIDTH as usize;
    let h = DISPLAY_HEIGHT as usize;
    let mut framebuffer = vec![0u32; w * h];

    let mut window = Window::new(
        "claudi-simulator",
        w,
        h,
        WindowOptions { scale: Scale::X2, resize: false, ..WindowOptions::default() },
    )?;
    window.set_target_fps(60);

    let mut rx_buf: Vec<u8> = Vec::with_capacity(16 * 1024);
    let mut last_mouse_down = false;
    let mut last_mouse_xy: Option<(u16, u16)> = None;
    let start = Instant::now();
    let mut last_rx = Instant::now();
    let idle_reset = std::time::Duration::from_millis(750);

    while window.is_open() && !window.is_key_down(Key::Escape) {
        // Read whatever is available
        let mut tmp = [0u8; 8192];
        let mut got = false;
        loop {
            match master.read(&mut tmp) {
                Ok(0) => break,
                Ok(n) => {
                    rx_buf.extend_from_slice(&tmp[..n]);
                    got = true;
                }
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => break,
                Err(e) => {
                    eprintln!("read error: {e}");
                    break;
                }
            }
        }
        if got {
            last_rx = Instant::now();
        } else if !rx_buf.is_empty() && last_rx.elapsed() > idle_reset {
            // Likely a stale fragment from a daemon that disconnected mid-frame; reset.
            rx_buf.clear();
        }
        // Decode frames
        loop {
            match Message::decode(&rx_buf) {
                Ok(Some((msg, n))) => {
                    apply(&msg, &mut framebuffer, w, h, &mut master);
                    rx_buf.drain(..n);
                }
                Ok(None) => break,
                Err(e) => {
                    eprintln!("decode error: {e} — clearing rx buffer");
                    rx_buf.clear();
                    break;
                }
            }
        }

        // Mouse → touch events
        if let Some((mx, my)) = window.get_mouse_pos(MouseMode::Pass) {
            let down = window.get_mouse_down(MouseButton::Left);
            let x = mx as i32;
            let y = my as i32;
            let in_bounds = x >= 0 && y >= 0 && (x as usize) < w && (y as usize) < h;
            if in_bounds {
                let xu = x as u16;
                let yu = y as u16;
                let ts = start.elapsed().as_millis() as u32;
                if down && !last_mouse_down {
                    let _ = write_msg(&mut master, &Message::Touch { phase: TouchPhase::Down, x: xu, y: yu, ts_ms: ts });
                    last_mouse_xy = Some((xu, yu));
                } else if down && last_mouse_down {
                    if last_mouse_xy != Some((xu, yu)) {
                        let _ = write_msg(&mut master, &Message::Touch { phase: TouchPhase::Move, x: xu, y: yu, ts_ms: ts });
                        last_mouse_xy = Some((xu, yu));
                    }
                } else if !down && last_mouse_down {
                    let _ = write_msg(&mut master, &Message::Touch { phase: TouchPhase::Up, x: xu, y: yu, ts_ms: ts });
                }
            }
            last_mouse_down = down;
        }

        window.update_with_buffer(&framebuffer, w, h)?;
    }

    Ok(())
}

fn open_pty() -> std::io::Result<(std::fs::File, String)> {
    use std::ffi::CStr;
    use std::os::fd::FromRawFd;
    unsafe {
        let master_fd = libc::posix_openpt(libc::O_RDWR | libc::O_NOCTTY);
        if master_fd < 0 {
            return Err(std::io::Error::last_os_error());
        }
        if libc::grantpt(master_fd) < 0 {
            return Err(std::io::Error::last_os_error());
        }
        if libc::unlockpt(master_fd) < 0 {
            return Err(std::io::Error::last_os_error());
        }
        let name_ptr = libc::ptsname(master_fd);
        if name_ptr.is_null() {
            return Err(std::io::Error::last_os_error());
        }
        let name = CStr::from_ptr(name_ptr).to_string_lossy().into_owned();
        // Hold the slave open so master reads don't fail when the daemon disconnects briefly.
        let slave_fd = libc::open(name_ptr, libc::O_RDWR | libc::O_NOCTTY);
        if slave_fd < 0 {
            return Err(std::io::Error::last_os_error());
        }
        // Put both ends into raw mode — no line discipline, no echo, no canonical mode.
        set_raw(master_fd)?;
        set_raw(slave_fd)?;
        let _ = slave_fd; // intentionally leaked to keep the pty alive
        Ok((std::fs::File::from_raw_fd(master_fd), name))
    }
}

unsafe fn set_raw(fd: libc::c_int) -> std::io::Result<()> {
    let mut tio: libc::termios = std::mem::zeroed();
    if libc::tcgetattr(fd, &mut tio) < 0 {
        return Err(std::io::Error::last_os_error());
    }
    libc::cfmakeraw(&mut tio);
    if libc::tcsetattr(fd, libc::TCSANOW, &tio) < 0 {
        return Err(std::io::Error::last_os_error());
    }
    Ok(())
}

fn set_nonblocking(f: &std::fs::File) -> std::io::Result<()> {
    use std::os::fd::AsRawFd;
    let fd = f.as_raw_fd();
    unsafe {
        let flags = libc::fcntl(fd, libc::F_GETFL);
        if flags < 0 {
            return Err(std::io::Error::last_os_error());
        }
        if libc::fcntl(fd, libc::F_SETFL, flags | libc::O_NONBLOCK) < 0 {
            return Err(std::io::Error::last_os_error());
        }
    }
    Ok(())
}

fn write_msg<W: Write>(w: &mut W, msg: &Message) -> std::io::Result<()> {
    let mut buf = Vec::new();
    msg.encode(&mut buf).map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e.to_string()))?;
    w.write_all(&buf)?;
    w.flush()
}

fn apply(msg: &Message, fb: &mut [u32], w: usize, h: usize, port: &mut std::fs::File) {
    match msg {
        Message::Hello { width, height, version } => {
            tracing::info!("hello v{version} {}x{}", width, height);
            let _ = write_msg(port, &Message::HelloAck {
                fw_version: 1,
                width: DISPLAY_WIDTH,
                height: DISPLAY_HEIGHT,
                features: 0b111,
            });
        }
        Message::Rect { x, y, w: rw, h: rh, pixels } => {
            blit(fb, w, h, *x as usize, *y as usize, *rw as usize, *rh as usize, pixels);
        }
        Message::RectRle { .. } => {
            // not used by current renderer
        }
        Message::FrameBegin { .. } | Message::FrameEnd => {}
        Message::Backlight { .. } => {}
        Message::Beep { freq_hz, duration_ms } => {
            tracing::info!("BEEP {} Hz / {} ms", freq_hz, duration_ms);
        }
        Message::SetTime { .. } => {}
        Message::Ping { seq } => {
            let _ = write_msg(port, &Message::Pong { seq: *seq });
        }
        _ => {}
    }
}

fn blit(fb: &mut [u32], fw: usize, fh: usize, x: usize, y: usize, w: usize, h: usize, pixels: &[u8]) {
    for row in 0..h {
        if y + row >= fh { break; }
        for col in 0..w {
            if x + col >= fw { break; }
            let i = (row * w + col) * 2;
            if i + 1 >= pixels.len() { break; }
            let lo = pixels[i] as u16;
            let hi = pixels[i + 1] as u16;
            let c = lo | (hi << 8);
            fb[(y + row) * fw + (x + col)] = claudi_protocol::rgb565_to_rgba(c);
        }
    }
}
