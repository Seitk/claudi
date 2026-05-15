//! 8x8 bitmap font, with a 2x scale variant for headers.
//!
//! Glyphs come from the public-domain `font8x8` crate. ASCII printable range
//! is covered; characters outside the BMP get rendered as a small box.

use font8x8::UnicodeFonts;

pub const GLYPH_W: u32 = 8;
pub const GLYPH_H: u32 = 8;

fn glyph_bits(c: char) -> [u8; 8] {
    font8x8::BASIC_FONTS
        .get(c)
        .or_else(|| font8x8::LATIN_FONTS.get(c))
        .unwrap_or([0x00, 0x66, 0x66, 0x00, 0x00, 0x66, 0x66, 0x00])
}

/// Draws `text` onto `buf` (width `bw`, height `bh`) starting at top-left (x, y).
/// `scale` is the integer pixel scale (1, 2, 3...).
///
/// Returns the x advance after drawing.
pub fn draw_text(
    buf: &mut [u16],
    bw: u32,
    bh: u32,
    x: i32,
    y: i32,
    scale: u32,
    color: u16,
    text: &str,
) -> i32 {
    let mut cx = x;
    let gw = (GLYPH_W * scale) as i32;
    let gh = (GLYPH_H * scale) as i32;
    for ch in text.chars() {
        if cx + gw < 0 || cx >= bw as i32 || y + gh < 0 || y >= bh as i32 {
            cx += gw;
            continue;
        }
        let bits = glyph_bits(ch);
        for (row, byte) in bits.iter().enumerate() {
            for col in 0..8 {
                let on = (byte >> col) & 1 != 0;
                if !on {
                    continue;
                }
                for dy in 0..scale {
                    for dx in 0..scale {
                        let px = cx + (col as u32 * scale + dx) as i32;
                        let py = y + (row as u32 * scale + dy) as i32;
                        if px >= 0 && py >= 0 && (px as u32) < bw && (py as u32) < bh {
                            buf[(py as u32 * bw + px as u32) as usize] = color;
                        }
                    }
                }
            }
        }
        cx += gw + scale as i32; // +scale = 1 px tracking at scale 1
    }
    cx
}

/// Width of `text` rendered at `scale`, in pixels (matches draw_text advances).
pub fn text_width(text: &str, scale: u32) -> i32 {
    let n = text.chars().count() as i32;
    n * ((GLYPH_W * scale) as i32 + scale as i32) - scale as i32
}

/// Convenience: draw text right-aligned to `x`.
pub fn draw_text_right(
    buf: &mut [u16],
    bw: u32,
    bh: u32,
    x_right: i32,
    y: i32,
    scale: u32,
    color: u16,
    text: &str,
) {
    let w = text_width(text, scale);
    draw_text(buf, bw, bh, x_right - w, y, scale, color, text);
}
