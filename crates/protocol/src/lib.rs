//! Wire protocol for claudi: Mac daemon <-> RP2350 firmware over USB CDC.
//!
//! Frame layout on the wire (little-endian throughout):
//!     [u8 type][u24 length][payload bytes...]
//!
//! `length` is the size of the payload (not including the 4-byte header).

use thiserror::Error;

pub const DISPLAY_WIDTH: u16 = 368;
pub const DISPLAY_HEIGHT: u16 = 448;
pub const PROTOCOL_VERSION: u16 = 1;

/// Max payload bytes per single message. Keeps device RX ring sane.
pub const MAX_PAYLOAD: usize = 64 * 1024;

#[derive(Debug, Error)]
pub enum CodecError {
    #[error("buffer too short: need {need}, have {have}")]
    Short { need: usize, have: usize },
    #[error("payload too large: {0} bytes")]
    TooLarge(usize),
    #[error("unknown message type: 0x{0:02x}")]
    UnknownType(u8),
    #[error("bad enum value for {field}: {value}")]
    BadEnum { field: &'static str, value: u8 },
    #[error("rect pixels mismatch: declared {decl}, payload {got}")]
    RectMismatch { decl: usize, got: usize },
}

// ----- message types -----

#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MsgType {
    Hello = 0x01,
    FrameBegin = 0x10,
    Rect = 0x11,
    RectRle = 0x12,
    FrameEnd = 0x13,
    Backlight = 0x20,
    Beep = 0x21,
    SetTime = 0x22,
    Reboot = 0x23,
    Ping = 0x30,

    HelloAck = 0x81,
    Touch = 0x90,
    Gesture = 0x91,
    Pong = 0xB0,
    Log = 0xC0,
}

impl MsgType {
    pub fn from_u8(v: u8) -> Result<Self, CodecError> {
        Ok(match v {
            0x01 => Self::Hello,
            0x10 => Self::FrameBegin,
            0x11 => Self::Rect,
            0x12 => Self::RectRle,
            0x13 => Self::FrameEnd,
            0x20 => Self::Backlight,
            0x21 => Self::Beep,
            0x22 => Self::SetTime,
            0x23 => Self::Reboot,
            0x30 => Self::Ping,
            0x81 => Self::HelloAck,
            0x90 => Self::Touch,
            0x91 => Self::Gesture,
            0xB0 => Self::Pong,
            0xC0 => Self::Log,
            other => return Err(CodecError::UnknownType(other)),
        })
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TouchPhase {
    Down,
    Move,
    Up,
}

impl TouchPhase {
    pub fn from_u8(v: u8) -> Result<Self, CodecError> {
        Ok(match v {
            0 => Self::Down,
            1 => Self::Move,
            2 => Self::Up,
            other => return Err(CodecError::BadEnum { field: "TouchPhase", value: other }),
        })
    }
    pub fn as_u8(self) -> u8 {
        match self {
            Self::Down => 0,
            Self::Move => 1,
            Self::Up => 2,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GestureKind {
    SwipeLeft,
    SwipeRight,
    SwipeUp,
    SwipeDown,
    Tap,
    LongPress,
}

impl GestureKind {
    pub fn from_u8(v: u8) -> Result<Self, CodecError> {
        Ok(match v {
            0 => Self::SwipeLeft,
            1 => Self::SwipeRight,
            2 => Self::SwipeUp,
            3 => Self::SwipeDown,
            4 => Self::Tap,
            5 => Self::LongPress,
            other => return Err(CodecError::BadEnum { field: "GestureKind", value: other }),
        })
    }
    pub fn as_u8(self) -> u8 {
        match self {
            Self::SwipeLeft => 0,
            Self::SwipeRight => 1,
            Self::SwipeUp => 2,
            Self::SwipeDown => 3,
            Self::Tap => 4,
            Self::LongPress => 5,
        }
    }
}

/// All messages, both directions.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Message {
    Hello { version: u16, width: u16, height: u16 },
    FrameBegin { frame_id: u32 },
    Rect { x: u16, y: u16, w: u16, h: u16, pixels: Vec<u8> },
    RectRle { x: u16, y: u16, w: u16, h: u16, data: Vec<u8> },
    FrameEnd,
    Backlight { level: u8 },
    Beep { freq_hz: u16, duration_ms: u16 },
    SetTime { unix_seconds: u64 },
    /// mode: 0 = soft reboot into app, 1 = reboot into BOOTSEL
    Reboot { mode: u8 },
    Ping { seq: u32 },

    HelloAck { fw_version: u16, width: u16, height: u16, features: u32 },
    Touch { phase: TouchPhase, x: u16, y: u16, ts_ms: u32 },
    Gesture { kind: GestureKind, dx: i16, dy: i16 },
    Pong { seq: u32 },
    Log { text: String },
}

impl Message {
    pub fn msg_type(&self) -> MsgType {
        match self {
            Self::Hello { .. } => MsgType::Hello,
            Self::FrameBegin { .. } => MsgType::FrameBegin,
            Self::Rect { .. } => MsgType::Rect,
            Self::RectRle { .. } => MsgType::RectRle,
            Self::FrameEnd => MsgType::FrameEnd,
            Self::Backlight { .. } => MsgType::Backlight,
            Self::Beep { .. } => MsgType::Beep,
            Self::SetTime { .. } => MsgType::SetTime,
            Self::Reboot { .. } => MsgType::Reboot,
            Self::Ping { .. } => MsgType::Ping,
            Self::HelloAck { .. } => MsgType::HelloAck,
            Self::Touch { .. } => MsgType::Touch,
            Self::Gesture { .. } => MsgType::Gesture,
            Self::Pong { .. } => MsgType::Pong,
            Self::Log { .. } => MsgType::Log,
        }
    }

    /// Encode a single message, header included, into `out`.
    pub fn encode(&self, out: &mut Vec<u8>) -> Result<(), CodecError> {
        let start = out.len();
        out.push(self.msg_type() as u8);
        // length placeholder (3 bytes, little-endian)
        out.extend_from_slice(&[0, 0, 0]);

        match self {
            Self::Hello { version, width, height } => {
                out.extend_from_slice(&version.to_le_bytes());
                out.extend_from_slice(&width.to_le_bytes());
                out.extend_from_slice(&height.to_le_bytes());
            }
            Self::FrameBegin { frame_id } => out.extend_from_slice(&frame_id.to_le_bytes()),
            Self::Rect { x, y, w, h, pixels } => {
                let expected = (*w as usize) * (*h as usize) * 2;
                if pixels.len() != expected {
                    return Err(CodecError::RectMismatch { decl: expected, got: pixels.len() });
                }
                out.extend_from_slice(&x.to_le_bytes());
                out.extend_from_slice(&y.to_le_bytes());
                out.extend_from_slice(&w.to_le_bytes());
                out.extend_from_slice(&h.to_le_bytes());
                out.extend_from_slice(pixels);
            }
            Self::RectRle { x, y, w, h, data } => {
                out.extend_from_slice(&x.to_le_bytes());
                out.extend_from_slice(&y.to_le_bytes());
                out.extend_from_slice(&w.to_le_bytes());
                out.extend_from_slice(&h.to_le_bytes());
                out.extend_from_slice(&(data.len() as u32).to_le_bytes());
                out.extend_from_slice(data);
            }
            Self::FrameEnd => {}
            Self::Backlight { level } => out.push(*level),
            Self::Beep { freq_hz, duration_ms } => {
                out.extend_from_slice(&freq_hz.to_le_bytes());
                out.extend_from_slice(&duration_ms.to_le_bytes());
            }
            Self::SetTime { unix_seconds } => out.extend_from_slice(&unix_seconds.to_le_bytes()),
            Self::Reboot { mode } => out.push(*mode),
            Self::Ping { seq } => out.extend_from_slice(&seq.to_le_bytes()),
            Self::HelloAck { fw_version, width, height, features } => {
                out.extend_from_slice(&fw_version.to_le_bytes());
                out.extend_from_slice(&width.to_le_bytes());
                out.extend_from_slice(&height.to_le_bytes());
                out.extend_from_slice(&features.to_le_bytes());
            }
            Self::Touch { phase, x, y, ts_ms } => {
                out.push(phase.as_u8());
                out.extend_from_slice(&x.to_le_bytes());
                out.extend_from_slice(&y.to_le_bytes());
                out.extend_from_slice(&ts_ms.to_le_bytes());
            }
            Self::Gesture { kind, dx, dy } => {
                out.push(kind.as_u8());
                out.extend_from_slice(&dx.to_le_bytes());
                out.extend_from_slice(&dy.to_le_bytes());
            }
            Self::Pong { seq } => out.extend_from_slice(&seq.to_le_bytes()),
            Self::Log { text } => out.extend_from_slice(text.as_bytes()),
        }

        let payload_len = out.len() - start - 4;
        if payload_len > MAX_PAYLOAD {
            return Err(CodecError::TooLarge(payload_len));
        }
        // backfill length (24-bit LE)
        out[start + 1] = (payload_len & 0xFF) as u8;
        out[start + 2] = ((payload_len >> 8) & 0xFF) as u8;
        out[start + 3] = ((payload_len >> 16) & 0xFF) as u8;
        Ok(())
    }

    /// Attempt to decode a single message from the front of `buf`.
    ///
    /// Returns `Ok(Some((msg, consumed)))` if a full message is present,
    /// `Ok(None)` if `buf` is incomplete, or `Err` on a malformed frame.
    pub fn decode(buf: &[u8]) -> Result<Option<(Message, usize)>, CodecError> {
        if buf.len() < 4 {
            return Ok(None);
        }
        let ty = MsgType::from_u8(buf[0])?;
        let len = (buf[1] as usize) | ((buf[2] as usize) << 8) | ((buf[3] as usize) << 16);
        if len > MAX_PAYLOAD {
            return Err(CodecError::TooLarge(len));
        }
        let total = 4 + len;
        if buf.len() < total {
            return Ok(None);
        }
        let p = &buf[4..total];

        fn need(p: &[u8], n: usize) -> Result<(), CodecError> {
            if p.len() < n {
                Err(CodecError::Short { need: n, have: p.len() })
            } else {
                Ok(())
            }
        }

        let msg = match ty {
            MsgType::Hello => {
                need(p, 6)?;
                Message::Hello {
                    version: u16::from_le_bytes([p[0], p[1]]),
                    width: u16::from_le_bytes([p[2], p[3]]),
                    height: u16::from_le_bytes([p[4], p[5]]),
                }
            }
            MsgType::FrameBegin => {
                need(p, 4)?;
                Message::FrameBegin { frame_id: u32::from_le_bytes([p[0], p[1], p[2], p[3]]) }
            }
            MsgType::Rect => {
                need(p, 8)?;
                let x = u16::from_le_bytes([p[0], p[1]]);
                let y = u16::from_le_bytes([p[2], p[3]]);
                let w = u16::from_le_bytes([p[4], p[5]]);
                let h = u16::from_le_bytes([p[6], p[7]]);
                let expected = (w as usize) * (h as usize) * 2;
                if p.len() - 8 != expected {
                    return Err(CodecError::RectMismatch { decl: expected, got: p.len() - 8 });
                }
                Message::Rect { x, y, w, h, pixels: p[8..].to_vec() }
            }
            MsgType::RectRle => {
                need(p, 12)?;
                let x = u16::from_le_bytes([p[0], p[1]]);
                let y = u16::from_le_bytes([p[2], p[3]]);
                let w = u16::from_le_bytes([p[4], p[5]]);
                let h = u16::from_le_bytes([p[6], p[7]]);
                let dlen = u32::from_le_bytes([p[8], p[9], p[10], p[11]]) as usize;
                if p.len() - 12 != dlen {
                    return Err(CodecError::RectMismatch { decl: dlen, got: p.len() - 12 });
                }
                Message::RectRle { x, y, w, h, data: p[12..].to_vec() }
            }
            MsgType::FrameEnd => Message::FrameEnd,
            MsgType::Backlight => {
                need(p, 1)?;
                Message::Backlight { level: p[0] }
            }
            MsgType::Beep => {
                need(p, 4)?;
                Message::Beep {
                    freq_hz: u16::from_le_bytes([p[0], p[1]]),
                    duration_ms: u16::from_le_bytes([p[2], p[3]]),
                }
            }
            MsgType::SetTime => {
                need(p, 8)?;
                let mut b = [0u8; 8];
                b.copy_from_slice(&p[..8]);
                Message::SetTime { unix_seconds: u64::from_le_bytes(b) }
            }
            MsgType::Reboot => {
                need(p, 1)?;
                Message::Reboot { mode: p[0] }
            }
            MsgType::Ping => {
                need(p, 4)?;
                Message::Ping { seq: u32::from_le_bytes([p[0], p[1], p[2], p[3]]) }
            }
            MsgType::HelloAck => {
                need(p, 10)?;
                Message::HelloAck {
                    fw_version: u16::from_le_bytes([p[0], p[1]]),
                    width: u16::from_le_bytes([p[2], p[3]]),
                    height: u16::from_le_bytes([p[4], p[5]]),
                    features: u32::from_le_bytes([p[6], p[7], p[8], p[9]]),
                }
            }
            MsgType::Touch => {
                need(p, 9)?;
                Message::Touch {
                    phase: TouchPhase::from_u8(p[0])?,
                    x: u16::from_le_bytes([p[1], p[2]]),
                    y: u16::from_le_bytes([p[3], p[4]]),
                    ts_ms: u32::from_le_bytes([p[5], p[6], p[7], p[8]]),
                }
            }
            MsgType::Gesture => {
                need(p, 5)?;
                Message::Gesture {
                    kind: GestureKind::from_u8(p[0])?,
                    dx: i16::from_le_bytes([p[1], p[2]]),
                    dy: i16::from_le_bytes([p[3], p[4]]),
                }
            }
            MsgType::Pong => {
                need(p, 4)?;
                Message::Pong { seq: u32::from_le_bytes([p[0], p[1], p[2], p[3]]) }
            }
            MsgType::Log => Message::Log {
                text: String::from_utf8_lossy(p).into_owned(),
            },
        };
        Ok(Some((msg, total)))
    }
}

// ----- RGB565 helpers -----

#[inline]
pub fn rgb565(r: u8, g: u8, b: u8) -> u16 {
    ((r as u16 >> 3) << 11) | ((g as u16 >> 2) << 5) | (b as u16 >> 3)
}

#[inline]
pub fn rgb565_from_u32(rgba: u32) -> u16 {
    let r = ((rgba >> 16) & 0xFF) as u8;
    let g = ((rgba >> 8) & 0xFF) as u8;
    let b = (rgba & 0xFF) as u8;
    rgb565(r, g, b)
}

#[inline]
pub fn rgb565_to_rgba(c: u16) -> u32 {
    let r = (((c >> 11) & 0x1F) as u32 * 255 / 31) & 0xFF;
    let g = (((c >> 5) & 0x3F) as u32 * 255 / 63) & 0xFF;
    let b = ((c & 0x1F) as u32 * 255 / 31) & 0xFF;
    0xFF00_0000 | (r << 16) | (g << 8) | b
}

#[cfg(test)]
mod tests {
    use super::*;

    fn roundtrip(m: Message) {
        let mut buf = Vec::new();
        m.encode(&mut buf).unwrap();
        let (back, consumed) = Message::decode(&buf).unwrap().unwrap();
        assert_eq!(consumed, buf.len());
        assert_eq!(m, back);
    }

    #[test]
    fn roundtrip_simple() {
        roundtrip(Message::Hello { version: 1, width: 368, height: 448 });
        roundtrip(Message::FrameBegin { frame_id: 12345 });
        roundtrip(Message::FrameEnd);
        roundtrip(Message::Backlight { level: 128 });
        roundtrip(Message::Beep { freq_hz: 880, duration_ms: 200 });
        roundtrip(Message::SetTime { unix_seconds: 1_700_000_000 });
        roundtrip(Message::Ping { seq: 7 });
        roundtrip(Message::Pong { seq: 7 });
        roundtrip(Message::HelloAck { fw_version: 1, width: 368, height: 448, features: 0b111 });
        roundtrip(Message::Log { text: "hello".into() });
    }

    #[test]
    fn roundtrip_touch() {
        roundtrip(Message::Touch { phase: TouchPhase::Down, x: 10, y: 20, ts_ms: 999 });
        roundtrip(Message::Touch { phase: TouchPhase::Move, x: 100, y: 200, ts_ms: 1000 });
        roundtrip(Message::Touch { phase: TouchPhase::Up, x: 50, y: 60, ts_ms: 1100 });
        roundtrip(Message::Gesture { kind: GestureKind::SwipeLeft, dx: -100, dy: 0 });
    }

    #[test]
    fn roundtrip_rect() {
        let w = 4u16;
        let h = 3u16;
        let pixels = vec![0xAB; (w as usize) * (h as usize) * 2];
        roundtrip(Message::Rect { x: 10, y: 20, w, h, pixels });
        roundtrip(Message::RectRle { x: 0, y: 0, w: 100, h: 100, data: vec![0x12, 0x34, 0x56] });
    }

    #[test]
    fn incomplete_returns_none() {
        let m = Message::Hello { version: 1, width: 368, height: 448 };
        let mut buf = Vec::new();
        m.encode(&mut buf).unwrap();
        // truncate
        let short = &buf[..3];
        assert!(Message::decode(short).unwrap().is_none());
        let short = &buf[..buf.len() - 1];
        assert!(Message::decode(short).unwrap().is_none());
    }

    #[test]
    fn unknown_type_errors() {
        let buf = [0xFFu8, 0, 0, 0];
        assert!(matches!(Message::decode(&buf), Err(CodecError::UnknownType(0xFF))));
    }

    #[test]
    fn rect_payload_mismatch_on_decode() {
        // declare w=2, h=2 (needs 8 pixel bytes) but only provide 4
        let mut buf = vec![MsgType::Rect as u8, 12, 0, 0]; // payload len = 8 + 4
        buf.extend_from_slice(&0u16.to_le_bytes()); // x
        buf.extend_from_slice(&0u16.to_le_bytes()); // y
        buf.extend_from_slice(&2u16.to_le_bytes()); // w
        buf.extend_from_slice(&2u16.to_le_bytes()); // h
        buf.extend_from_slice(&[0; 4]); // wrong size (need 8)
        assert!(matches!(Message::decode(&buf), Err(CodecError::RectMismatch { .. })));
    }

    #[test]
    fn rgb565_roundtrip_close() {
        // 5-6-5 isn't lossless, but should preserve to within a couple bits per channel
        for (r, g, b) in [(0, 0, 0), (255, 255, 255), (255, 0, 0), (0, 255, 0), (0, 0, 255)] {
            let c = rgb565(r, g, b);
            let back = rgb565_to_rgba(c);
            let br = ((back >> 16) & 0xFF) as i32;
            let bg = ((back >> 8) & 0xFF) as i32;
            let bb = (back & 0xFF) as i32;
            assert!((br - r as i32).abs() <= 8, "r mismatch: {} -> {}", r, br);
            assert!((bg - g as i32).abs() <= 4, "g mismatch: {} -> {}", g, bg);
            assert!((bb - b as i32).abs() <= 8, "b mismatch: {} -> {}", b, bb);
        }
    }

    #[test]
    fn two_messages_back_to_back() {
        let mut buf = Vec::new();
        Message::Ping { seq: 1 }.encode(&mut buf).unwrap();
        Message::Pong { seq: 2 }.encode(&mut buf).unwrap();
        let (m1, c1) = Message::decode(&buf).unwrap().unwrap();
        let (m2, c2) = Message::decode(&buf[c1..]).unwrap().unwrap();
        assert_eq!(m1, Message::Ping { seq: 1 });
        assert_eq!(m2, Message::Pong { seq: 2 });
        assert_eq!(c1 + c2, buf.len());
    }
}
