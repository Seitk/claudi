//! Renderer for the device screens.
//!
//! Produces RGB565 framebuffers and emits dirty rects against a back-buffer
//! cache so we only ship the changed regions over USB.

use crate::font;
use crate::state::{DaemonState, Screen, Status};
use claudi_protocol::{rgb565, DISPLAY_HEIGHT, DISPLAY_WIDTH};

pub const STATUS_BAR_HEIGHT: u16 = 24;
pub const SESSION_ROW_HEIGHT: u16 = 72;
pub const FEED_ROW_HEIGHT: u16 = 26;

// Palette (Tokyo Night–ish, AMOLED-friendly with deep blacks)
fn bg() -> u16 { rgb565(0x0A, 0x0C, 0x12) }
fn fg() -> u16 { rgb565(0xE5, 0xE9, 0xF0) }
fn dim() -> u16 { rgb565(0x6A, 0x71, 0x85) }
fn accent() -> u16 { rgb565(0x7A, 0xA2, 0xF7) }
fn ok() -> u16 { rgb565(0x9E, 0xCE, 0x6A) }
fn warn() -> u16 { rgb565(0xE0, 0xAF, 0x68) }
fn danger() -> u16 { rgb565(0xF7, 0x76, 0x8E) }
fn panel() -> u16 { rgb565(0x14, 0x18, 0x22) }

pub struct DirtyRect {
    pub x: u16,
    pub y: u16,
    pub w: u16,
    pub h: u16,
    pub pixels: Vec<u8>, // RGB565 little-endian
}

pub struct Frame {
    pub rects: Vec<DirtyRect>,
}

/// Render the current state to a PNG (used by `claudi render-png` for debugging).
pub fn render_png_snapshot(state: &DaemonState) -> Vec<u8> {
    let w = DISPLAY_WIDTH as u32;
    let h = DISPLAY_HEIGHT as u32;
    let mut back = vec![bg(); (w * h) as usize];
    draw_status_bar(&mut back, w, h, state);
    match state.screen {
        Screen::Sessions => draw_sessions(&mut back, w, h, state),
        Screen::Feed => draw_feed(&mut back, w, h, state),
        Screen::Metrics => draw_metrics(&mut back, w, h, state),
        Screen::Approval => {
            draw_sessions(&mut back, w, h, state);
            draw_approval(&mut back, w, h, state);
        }
    }
    draw_page_indicator(&mut back, w, h, state);

    // RGB565 -> 8-bit RGB
    let mut rgb = Vec::with_capacity(back.len() * 3);
    for px in &back {
        let rgba = claudi_protocol::rgb565_to_rgba(*px);
        rgb.push(((rgba >> 16) & 0xFF) as u8);
        rgb.push(((rgba >> 8) & 0xFF) as u8);
        rgb.push((rgba & 0xFF) as u8);
    }

    let mut png_bytes = Vec::new();
    {
        let mut encoder = png::Encoder::new(&mut png_bytes, w, h);
        encoder.set_color(png::ColorType::Rgb);
        encoder.set_depth(png::BitDepth::Eight);
        let mut writer = encoder.write_header().expect("png header");
        writer.write_image_data(&rgb).expect("png data");
    }
    png_bytes
}

pub struct Renderer {
    front: Vec<u16>,   // last shipped framebuffer
    back: Vec<u16>,    // next frame
    width: u32,
    height: u32,
    first: bool,
}

impl Renderer {
    pub fn new() -> Self {
        let w = DISPLAY_WIDTH as u32;
        let h = DISPLAY_HEIGHT as u32;
        Self {
            front: vec![0; (w * h) as usize],
            back: vec![0; (w * h) as usize],
            width: w,
            height: h,
            first: true,
        }
    }

    pub fn render_frame(&mut self, state: &DaemonState) -> Frame {
        // clear back-buffer
        for px in self.back.iter_mut() {
            *px = bg();
        }
        draw_status_bar(&mut self.back, self.width, self.height, state);
        match state.screen {
            Screen::Sessions => draw_sessions(&mut self.back, self.width, self.height, state),
            Screen::Feed => draw_feed(&mut self.back, self.width, self.height, state),
            Screen::Metrics => draw_metrics(&mut self.back, self.width, self.height, state),
            Screen::Approval => {
                // base screen below modal
                draw_sessions(&mut self.back, self.width, self.height, state);
                draw_approval(&mut self.back, self.width, self.height, state);
            }
        }
        draw_page_indicator(&mut self.back, self.width, self.height, state);

        let force_all = self.first;
        self.first = false;
        let rects = diff_into_rects(&self.front, &self.back, self.width, self.height, force_all);
        std::mem::swap(&mut self.front, &mut self.back);
        Frame { rects }
    }
}

/// Coarse 32x32 tile diff to keep dirty rect count small.
fn diff_into_rects(front: &[u16], back: &[u16], w: u32, h: u32, force_all: bool) -> Vec<DirtyRect> {
    let tile = 32u32;
    let mut rects = Vec::new();
    let mut y = 0u32;
    while y < h {
        let th = tile.min(h - y);
        let mut x = 0u32;
        while x < w {
            let tw = tile.min(w - x);
            if force_all || tile_dirty(front, back, w, x, y, tw, th) {
                let mut pixels = Vec::with_capacity((tw * th * 2) as usize);
                for row in 0..th {
                    let off = ((y + row) * w + x) as usize;
                    for px in &back[off..off + tw as usize] {
                        pixels.extend_from_slice(&px.to_le_bytes());
                    }
                }
                rects.push(DirtyRect {
                    x: x as u16, y: y as u16, w: tw as u16, h: th as u16, pixels,
                });
            }
            x += tile;
        }
        y += tile;
    }
    rects
}

fn tile_dirty(a: &[u16], b: &[u16], w: u32, x: u32, y: u32, tw: u32, th: u32) -> bool {
    for row in 0..th {
        let off = ((y + row) * w + x) as usize;
        if a[off..off + tw as usize] != b[off..off + tw as usize] {
            return true;
        }
    }
    false
}

fn fill_rect(buf: &mut [u16], w: u32, h: u32, x: i32, y: i32, rw: i32, rh: i32, color: u16) {
    let x0 = x.max(0) as u32;
    let y0 = y.max(0) as u32;
    let x1 = ((x + rw).max(0) as u32).min(w);
    let y1 = ((y + rh).max(0) as u32).min(h);
    for yy in y0..y1 {
        for xx in x0..x1 {
            buf[(yy * w + xx) as usize] = color;
        }
    }
}

fn draw_hline(buf: &mut [u16], w: u32, h: u32, x: i32, y: i32, len: i32, color: u16) {
    fill_rect(buf, w, h, x, y, len, 1, color);
}

fn draw_status_bar(buf: &mut [u16], w: u32, h: u32, state: &DaemonState) {
    fill_rect(buf, w, h, 0, 0, w as i32, STATUS_BAR_HEIGHT as i32, panel());
    // "claudi"
    font::draw_text(buf, w, h, 8, 8, 1, accent(), "claudi");
    // dot + count
    let active = state.sessions.values().filter(|s| matches!(s.status, Status::Working { .. } | Status::Waiting)).count();
    let total = state.sessions.len();
    let dot = if active > 0 { ok() } else { dim() };
    fill_rect(buf, w, h, 64, 9, 6, 6, dot);
    let label = if total == 1 {
        "1 session".to_string()
    } else {
        format!("{total} sessions")
    };
    font::draw_text(buf, w, h, 76, 8, 1, fg(), &label);
    // clock right-aligned
    let now = chrono::Local::now().format("%H:%M").to_string();
    font::draw_text_right(buf, w, h, w as i32 - 8, 8, 1, fg(), &now);
    // device indicator — small offline dot near the clock, doesn't overlap labels
    if !state.device_connected {
        fill_rect(buf, w, h, w as i32 - 56, 9, 6, 6, danger());
    }
}

fn draw_page_indicator(buf: &mut [u16], w: u32, h: u32, state: &DaemonState) {
    if matches!(state.screen, Screen::Approval) {
        return;
    }
    let active_color = accent();
    let inactive_color = dim();
    let pages = [Screen::Sessions, Screen::Feed, Screen::Metrics];
    let total = pages.len() as i32;
    let spacing = 14;
    let total_w = (total - 1) * spacing;
    let cx = w as i32 / 2 - total_w / 2;
    let y = h as i32 - 10;
    for (i, p) in pages.iter().enumerate() {
        let c = if *p == state.screen { active_color } else { inactive_color };
        fill_rect(buf, w, h, cx + (i as i32) * spacing - 3, y, 6, 6, c);
    }
}

fn draw_sessions(buf: &mut [u16], w: u32, h: u32, state: &DaemonState) {
    let mut y = STATUS_BAR_HEIGHT as i32 + 8;
    if state.sessions.is_empty() {
        let msg = "no active sessions";
        let x = (w as i32) / 2 - font::text_width(msg, 2) / 2;
        font::draw_text(buf, w, h, x, h as i32 / 2 - 8, 2, dim(), msg);
        return;
    }

    // sort by last_activity desc (most recent first)
    let mut sessions: Vec<&crate::state::SessionState> = state.sessions.values().collect();
    sessions.sort_by(|a, b| b.last_activity.cmp(&a.last_activity));

    for sess in sessions.iter().take(5) {
        draw_session_card(buf, w, h, 8, y, w as i32 - 16, SESSION_ROW_HEIGHT as i32 - 4, sess);
        y += SESSION_ROW_HEIGHT as i32;
        if y + (SESSION_ROW_HEIGHT as i32) > h as i32 - 18 {
            break;
        }
    }
}

fn draw_session_card(
    buf: &mut [u16],
    w: u32,
    h: u32,
    x: i32,
    y: i32,
    cw: i32,
    ch: i32,
    sess: &crate::state::SessionState,
) {
    fill_rect(buf, w, h, x, y, cw, ch, panel());
    // status pill
    let (pill_color, label) = match &sess.status {
        Status::Idle => (dim(), "idle"),
        Status::Working { .. } => (ok(), "work"),
        Status::Waiting => (warn(), "wait"),
        Status::Stopped => (dim(), "done"),
        Status::Starting => (accent(), "init"),
    };
    fill_rect(buf, w, h, x + 8, y + 8, 8, 8, pill_color);
    // session short name
    font::draw_text(buf, w, h, x + 24, y + 6, 2, fg(), &sess.short_name());
    // status text
    font::draw_text(buf, w, h, x + 24, y + 28, 1, dim(), label);
    // last activity preview
    if let Some(top) = sess.feed.front() {
        let preview = truncate_text(&top.summary, 32);
        font::draw_text(buf, w, h, x + 24, y + 44, 1, fg(), &preview);
    }
    // tokens hint right side
    let toks = sess.usage.input_tokens + sess.usage.output_tokens;
    if toks > 0 {
        let label = format_tokens(toks);
        font::draw_text_right(buf, w, h, x + cw - 8, y + 6, 1, accent(), &label);
    }
}

fn draw_feed(buf: &mut [u16], w: u32, h: u32, state: &DaemonState) {
    let header_y = STATUS_BAR_HEIGHT as i32 + 4;
    let sess = match state.focused_session().or_else(|| state.sessions.values().next()) {
        Some(s) => s,
        None => {
            let msg = "no session";
            let x = (w as i32) / 2 - font::text_width(msg, 2) / 2;
            font::draw_text(buf, w, h, x, h as i32 / 2 - 8, 2, dim(), msg);
            return;
        }
    };
    font::draw_text(buf, w, h, 8, header_y, 2, fg(), &sess.short_name());
    let mut y = header_y + 24;
    draw_hline(buf, w, h, 8, y, w as i32 - 16, dim());
    y += 6;
    for item in sess.feed.iter().take(((h as i32 - y - 18) / FEED_ROW_HEIGHT as i32) as usize) {
        // tool tag color
        let tag_color = match item.tool.as_str() {
            "Bash" => warn(),
            "Edit" | "Write" => accent(),
            "Read" | "Grep" | "Glob" => dim(),
            "Task" | "Agent" => ok(),
            "user" => fg(),
            "notify" => warn(),
            _ => fg(),
        };
        let t = chrono_time(&item.at);
        font::draw_text(buf, w, h, 8, y, 1, dim(), &t);
        font::draw_text(buf, w, h, 8 + 45, y, 1, tag_color, &truncate_text(&item.tool, 6));
        font::draw_text(buf, w, h, 8 + 45 + 65, y, 1, fg(), &truncate_text(&item.summary, 32));
        y += FEED_ROW_HEIGHT as i32;
    }
}

fn draw_approval(buf: &mut [u16], w: u32, h: u32, state: &DaemonState) {
    // scrim
    fill_rect(buf, w, h, 0, 0, w as i32, h as i32, rgb565(0, 0, 0));
    let pad = 16;
    let card_x = pad;
    let card_y = pad + STATUS_BAR_HEIGHT as i32;
    let card_w = w as i32 - pad * 2;
    let card_h = h as i32 - card_y - pad;
    fill_rect(buf, w, h, card_x, card_y, card_w, card_h, panel());

    // header
    font::draw_text(buf, w, h, card_x + 12, card_y + 12, 2, warn(), "approval");
    if let Some(pa) = &state.pending_approval {
        font::draw_text(buf, w, h, card_x + 12, card_y + 40, 1, dim(), &pa.session_id[..pa.session_id.len().min(8)]);
        font::draw_text(buf, w, h, card_x + 12, card_y + 56, 1, fg(), &format!("tool: {}", pa.tool));

        let mut y = card_y + 80;
        for chunk in wrap_text(&pa.command_or_args, 36) {
            font::draw_text(buf, w, h, card_x + 12, y, 1, fg(), &chunk);
            y += 14;
            if y > card_y + card_h - 130 { break; }
        }

        // buttons row at bottom — scale 2 keeps the labels inside their button
        let btn_h = 80;
        let btn_y = card_y + card_h - btn_h - 12;
        let half = card_w / 2;
        let btn_w = half - 12;
        let lbl_scale = 2;
        // DENY (left)
        fill_rect(buf, w, h, card_x + 8, btn_y, btn_w, btn_h, danger());
        let dl = "DENY";
        let dx = card_x + 8 + btn_w / 2 - font::text_width(dl, lbl_scale) / 2;
        font::draw_text(buf, w, h, dx, btn_y + btn_h / 2 - 8, lbl_scale, bg(), dl);
        // APPROVE (right)
        fill_rect(buf, w, h, card_x + half + 4, btn_y, btn_w, btn_h, ok());
        let al = "APPROVE";
        let ax = card_x + half + 4 + btn_w / 2 - font::text_width(al, lbl_scale) / 2;
        font::draw_text(buf, w, h, ax, btn_y + btn_h / 2 - 8, lbl_scale, bg(), al);

        // countdown
        let remaining = pa.timeout.saturating_sub(pa.created_at.elapsed()).as_secs();
        let cd = format!("auto-deny in {}s", remaining);
        font::draw_text(buf, w, h, card_x + 12, btn_y - 18, 1, dim(), &cd);
    }
}

fn draw_metrics(buf: &mut [u16], w: u32, h: u32, state: &DaemonState) {
    let mut y = STATUS_BAR_HEIGHT as i32 + 12;
    font::draw_text(buf, w, h, 8, y, 2, fg(), "today");
    y += 28;
    // aggregate
    let mut total_in: u64 = 0;
    let mut total_out: u64 = 0;
    let mut total_cost: f64 = 0.0;
    let mut total_calls: u64 = 0;
    let mut by_model: std::collections::HashMap<String, u64> = std::collections::HashMap::new();
    for s in state.sessions.values() {
        total_in += s.usage.input_tokens;
        total_out += s.usage.output_tokens;
        total_cost += s.usage.cost_usd;
        total_calls += s.feed.iter().filter(|f| f.tool != "user" && f.tool != "session" && f.tool != "notify" && f.tool != "stop").count() as u64;
        for (m, n) in &s.usage.by_model {
            *by_model.entry(m.clone()).or_insert(0) += n;
        }
    }
    metric_row(buf, w, h, 8, y, "tokens in", &format_tokens(total_in));
    y += 18;
    metric_row(buf, w, h, 8, y, "tokens out", &format_tokens(total_out));
    y += 18;
    metric_row(buf, w, h, 8, y, "cost", &format!("${:.2}", total_cost));
    y += 18;
    metric_row(buf, w, h, 8, y, "sessions", &state.sessions.len().to_string());
    y += 18;
    metric_row(buf, w, h, 8, y, "tool calls", &total_calls.to_string());
    y += 24;
    draw_hline(buf, w, h, 8, y, w as i32 - 16, dim());
    y += 6;
    font::draw_text(buf, w, h, 8, y, 1, dim(), "models");
    y += 14;
    let mut models: Vec<(&String, &u64)> = by_model.iter().collect();
    models.sort_by(|a, b| b.1.cmp(a.1));
    for (m, n) in models.iter().take(5) {
        fill_rect(buf, w, h, 8, y + 3, 4, 4, accent());
        font::draw_text(buf, w, h, 18, y, 1, fg(), &truncate_text(m, 28));
        font::draw_text_right(buf, w, h, w as i32 - 8, y, 1, dim(), &format_tokens(**n));
        y += 14;
    }
}

fn metric_row(buf: &mut [u16], w: u32, h: u32, x: i32, y: i32, label: &str, value: &str) {
    font::draw_text(buf, w, h, x, y, 1, dim(), label);
    font::draw_text_right(buf, w, h, w as i32 - 8, y, 1, fg(), value);
}

fn truncate_text(s: &str, n: usize) -> String {
    if s.chars().count() <= n {
        s.into()
    } else {
        let mut out: String = s.chars().take(n.saturating_sub(1)).collect();
        out.push('…');
        out
    }
}

fn wrap_text(s: &str, cols: usize) -> Vec<String> {
    let mut out = Vec::new();
    let mut cur = String::new();
    for word in s.split_whitespace() {
        if !cur.is_empty() && cur.len() + 1 + word.len() > cols {
            out.push(std::mem::take(&mut cur));
        }
        if !cur.is_empty() {
            cur.push(' ');
        }
        cur.push_str(word);
    }
    if !cur.is_empty() {
        out.push(cur);
    }
    if out.is_empty() {
        out.push(String::new());
    }
    out
}

fn format_tokens(n: u64) -> String {
    if n >= 1_000_000 {
        format!("{:.1}M", n as f64 / 1_000_000.0)
    } else if n >= 1_000 {
        format!("{:.1}K", n as f64 / 1_000.0)
    } else {
        n.to_string()
    }
}

fn chrono_time(t: &std::time::SystemTime) -> String {
    let dt: chrono::DateTime<chrono::Local> = (*t).into();
    dt.format("%H:%M").to_string()
}

