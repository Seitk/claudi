#!/usr/bin/env python3
"""Generate flat, anti-aliased launcher icons for the games as LVGL ARGB8888 C
images. No third-party deps — shapes are drawn with signed-distance fields and
1px coverage AA, then emitted in the exact lv_image_dsc_t form the launcher reads
(it auto-scales the source, so 112x112 — matching the stock default icon — is fine).

Run:  python3 firmware/tools/gen_game_icons.py
Writes: firmware/components/game_icons/icon_<name>.c  (+ game_icons.h is static)
"""
import math
import os

W = H = 112
HERE = os.path.dirname(os.path.abspath(__file__))
OUTDIR = os.path.join(os.path.dirname(HERE), "components", "game_icons")


# ---- tiny SDF drawing toolkit (acc holds straight RGBA, rgb 0..255, a 0..1) ----
def new_acc():
    return [[[0.0, 0.0, 0.0, 0.0] for _ in range(W)] for _ in range(H)]


def put(acc, x, y, rgb, cov):
    if cov <= 0.0:
        return
    if cov > 1.0:
        cov = 1.0
    px = acc[y][x]
    dr, dg, db, da = px
    na = cov + da * (1.0 - cov)
    if na <= 1e-6:
        px[0] = px[1] = px[2] = px[3] = 0.0
        return
    px[0] = (rgb[0] * cov + dr * da * (1.0 - cov)) / na
    px[1] = (rgb[1] * cov + dg * da * (1.0 - cov)) / na
    px[2] = (rgb[2] * cov + db * da * (1.0 - cov)) / na
    px[3] = na


def draw(acc, sdf, rgb, alpha=1.0):
    """Fill where sdf(px,py) < 0, with 1px anti-aliased coverage."""
    for y in range(H):
        fy = y + 0.5
        row = acc[y]
        for x in range(W):
            d = sdf(x + 0.5, fy)
            cov = 0.5 - d
            if cov <= 0.0:
                continue
            if cov > 1.0:
                cov = 1.0
            cov *= alpha
            if cov <= 0.0:
                continue
            # inlined put for speed
            dr, dg, db, da = row[x]
            na = cov + da * (1.0 - cov)
            if na <= 1e-6:
                row[x] = [0.0, 0.0, 0.0, 0.0]
                continue
            row[x] = [
                (rgb[0] * cov + dr * da * (1.0 - cov)) / na,
                (rgb[1] * cov + dg * da * (1.0 - cov)) / na,
                (rgb[2] * cov + db * da * (1.0 - cov)) / na,
                na,
            ]


def hexrgb(c):
    return ((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF)


def sdf_circle(cx, cy, r):
    return lambda px, py: math.hypot(px - cx, py - cy) - r


def sdf_ring(cx, cy, r, w):
    return lambda px, py: abs(math.hypot(px - cx, py - cy) - r) - w * 0.5


def sdf_rrect(cx, cy, hw, hh, rad):
    def f(px, py):
        qx = abs(px - cx) - (hw - rad)
        qy = abs(py - cy) - (hh - rad)
        return (math.hypot(max(qx, 0.0), max(qy, 0.0))
                + min(max(qx, qy), 0.0) - rad)
    return f


def sdf_segment(ax, ay, bx, by, thick):
    dx, dy = bx - ax, by - ay
    ll = dx * dx + dy * dy

    def f(px, py):
        if ll <= 1e-6:
            t = 0.0
        else:
            t = ((px - ax) * dx + (py - ay) * dy) / ll
            t = max(0.0, min(1.0, t))
        cxp, cyp = ax + t * dx, ay + t * dy
        return math.hypot(px - cxp, py - cyp) - thick * 0.5
    return f


def sdf_arc(cx, cy, r, w, a0, a1):
    """Ring limited to the angular wedge [a0, a1] (radians, standard math)."""
    def f(px, py):
        ang = math.atan2(py - cy, px - cx)
        # normalize ang into [a0, a0+2pi)
        a = ang
        while a < a0:
            a += 2 * math.pi
        if a > a1:
            # outside the wedge: distance to the nearer endpoint cap
            e0 = (cx + r * math.cos(a0), cy + r * math.sin(a0))
            e1 = (cx + r * math.cos(a1), cy + r * math.sin(a1))
            d0 = math.hypot(px - e0[0], py - e0[1]) - w * 0.5
            d1 = math.hypot(px - e1[0], py - e1[1]) - w * 0.5
            return min(d0, d1)
        return abs(math.hypot(px - cx, py - cy) - r) - w * 0.5
    return f


def sdf_convex(points):
    """Signed distance to a convex polygon (CCW), negative inside."""
    n = len(points)

    def f(px, py):
        inside = True
        dmin = 1e9
        for i in range(n):
            ax, ay = points[i]
            bx, by = points[(i + 1) % n]
            ex, ey = bx - ax, by - ay
            wx, wy = px - ax, py - ay
            ll = ex * ex + ey * ey
            t = 0.0 if ll <= 1e-6 else max(0.0, min(1.0, (wx * ex + wy * ey) / ll))
            cxp, cyp = ax + t * ex, ay + t * ey
            dmin = min(dmin, math.hypot(px - cxp, py - cyp))
            # cross product sign (CCW => inside if >=0)
            if (ex * wy - ey * wx) < 0:
                inside = False
        return -dmin if inside else dmin
    return f


# ----------------------------- the six icons --------------------------------
def bg(acc, color):
    draw(acc, sdf_rrect(56, 56, 53, 53, 26), hexrgb(color), 1.0)


def icon_bullseye(acc):
    bg(acc, 0x0E1530)
    draw(acc, sdf_ring(56, 56, 40, 8), hexrgb(0x29C7C7))
    draw(acc, sdf_ring(56, 56, 26, 8), hexrgb(0xEAF6FF))
    draw(acc, sdf_circle(56, 56, 13), hexrgb(0xE8533F))


def icon_tappop(acc):
    bg(acc, 0x2A1E4A)
    # popper body
    draw(acc, sdf_circle(56, 52, 34), hexrgb(0xFF6B6B))
    draw(acc, sdf_circle(46, 42, 9), hexrgb(0x29C7C7))  # tiny shine
    # eyes
    draw(acc, sdf_circle(46, 50, 8), hexrgb(0xFFFFFF))
    draw(acc, sdf_circle(66, 50, 8), hexrgb(0xFFFFFF))
    draw(acc, sdf_circle(47, 51, 3.5), hexrgb(0x20131A))
    draw(acc, sdf_circle(67, 51, 3.5), hexrgb(0x20131A))
    # smile (upward-open arc, lower face)
    draw(acc, sdf_arc(56, 56, 14, 4, math.radians(25), math.radians(155)), hexrgb(0x20131A))
    # a little sparkle
    draw(acc, sdf_segment(86, 26, 86, 38, 4), hexrgb(0xFFD93B))
    draw(acc, sdf_segment(80, 32, 92, 32, 4), hexrgb(0xFFD93B))


def icon_colormatch(acc):
    bg(acc, 0x101018)
    pads = [(-20, -20, 0xFF4D4D), (20, -20, 0x4CD964),
            (-20, 20, 0x4D9DFF), (20, 20, 0xFFD93B)]
    for dx, dy, col in pads:
        draw(acc, sdf_rrect(56 + dx, 56 + dy, 18, 18, 8), hexrgb(col))


def icon_doodle(acc):
    bg(acc, 0xF5F5F2)
    # rainbow squiggle (a sine wave sampled into colored segments)
    cols = [0xFF3B30, 0xFF9500, 0x34C759, 0x0A84FF, 0xBF5AF2, 0xFF2D86]
    pts = []
    for i in range(25):
        t = i / 24.0
        x = 22 + t * 68
        y = 60 + 22 * math.sin(t * math.pi * 2.4)
        pts.append((x, y))
    for i in range(len(pts) - 1):
        col = cols[i % len(cols)]
        draw(acc, sdf_segment(pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1], 8), hexrgb(col))
    # round the ends/joins with dots
    for (x, y), col in zip(pts[::4], cols):
        draw(acc, sdf_circle(x, y, 4), hexrgb(col))


def icon_fruitninja(acc):
    bg(acc, 0x0A0A12)
    # watermelon-ish fruit
    draw(acc, sdf_circle(54, 62, 33), hexrgb(0x3FB950))   # rind
    draw(acc, sdf_circle(54, 62, 25), hexrgb(0xFF5252))   # flesh
    for sx, sy in [(46, 58), (60, 54), (54, 68), (64, 66), (44, 70)]:
        draw(acc, sdf_circle(sx, sy, 2.4), hexrgb(0x20131A))  # seeds
    # blade slash across the top-left to bottom-right
    draw(acc, sdf_segment(20, 30, 92, 78, 9), hexrgb(0xEAF6FF), 0.92)
    draw(acc, sdf_segment(20, 30, 92, 78, 3), hexrgb(0xFFFFFF))


def icon_voiceplane(acc):
    # "Sky Hop": a plane flying through a gap in green pillars.
    bg(acc, 0x1E3A6E)
    # obstacle pillars (top + bottom) with a gap, on the right
    draw(acc, sdf_rrect(84, 20, 11, 26, 5), hexrgb(0x4CD964))   # top pillar
    draw(acc, sdf_rrect(84, 90, 11, 24, 5), hexrgb(0x4CD964))   # bottom pillar
    # paper plane pointing right (filled triangle + fold)
    tri = [(66, 56), (28, 40), (28, 72)]
    draw(acc, sdf_convex(tri), hexrgb(0xFFC400))
    draw(acc, sdf_segment(66, 56, 28, 56, 3), hexrgb(0xCC8E00))  # center fold


# ---- pixel-art helpers (crisp square "pixels" for the new pixelated games) ----
def pxrect(acc, x0, y0, w, h, color, alpha=1.0):
    """Fill a single square pixel block with its top-left at (x0, y0)."""
    draw(acc, sdf_rrect(x0 + w / 2.0, y0 + h / 2.0, w / 2.0, h / 2.0, 0.0),
         hexrgb(color), alpha)


def draw_pixmap(acc, ox, oy, cell, rows, palette, inset=0.0):
    """Render an ASCII pixel map: each non-blank char draws a `cell`-sized block
    colored by `palette[char]`. '.'/' ' are empty. `inset` shrinks each block to
    leave thin grid gaps."""
    for j, row in enumerate(rows):
        for i, ch in enumerate(row):
            if ch == "." or ch == " ":
                continue
            pxrect(acc, ox + i * cell + inset, oy + j * cell + inset,
                   cell - 2 * inset, cell - 2 * inset, palette[ch])


# --------------------------- the five pixel games ---------------------------
def icon_pixelreflex(acc):
    # Tap game: a 3x3 grid of cells with one lit bright — "tap the lit one".
    bg(acc, 0x161024)
    cell, gap = 22, 6
    span = 3 * cell + 2 * gap
    ox = oy = 56 - span / 2.0
    lit = (1, 1)  # center cell glows
    for r in range(3):
        for c in range(3):
            color = 0xFFD93B if (c, r) == lit else 0x33304D
            pxrect(acc, ox + c * (cell + gap), oy + r * (cell + gap), cell, cell, color)
    # a tiny tap-spark on the lit cell
    cx = ox + cell + gap + cell / 2.0
    cy = oy + cell + gap + cell / 2.0
    draw(acc, sdf_circle(cx, cy, 5), hexrgb(0xFFFFFF))


def icon_pixelsnake(acc):
    # Touch/drag game: a green pixel snake chasing a red apple.
    bg(acc, 0x0C1A10)
    cell, ox, oy = 14, 14, 14
    rows = [
        "      ",
        ".HHHh.",
        ".H....",
        ".H..A.",
        ".HHH..",
        "      ",
    ]
    palette = {"H": 0x3FB950, "h": 0x9DFFB0, "A": 0xFF5252}
    draw_pixmap(acc, ox, oy, cell, rows, palette, inset=1.0)
    # dark eye on the head cell (top-right of the body run)
    draw(acc, sdf_circle(14 + 4 * cell + cell / 2.0, 14 + 1 * cell + cell / 2.0, 2.6),
         hexrgb(0x0C1A10))


def icon_pixel2048(acc):
    # Swipe game: warm 2048-style number tiles + a swipe arrow.
    bg(acc, 0x1B1712)
    cell, gap = 30, 5
    span = 2 * cell + gap
    ox = 56 - span / 2.0
    oy = 18
    tiles = [0xEEE4DA, 0xEDE0C8, 0xF2B179, 0xF59563]
    k = 0
    for r in range(2):
        for c in range(2):
            pxrect(acc, ox + c * (cell + gap), oy + r * (cell + gap), cell, cell, tiles[k])
            k += 1
    # white right-swipe arrow below the tiles
    draw(acc, sdf_segment(38, 94, 62, 94, 7), hexrgb(0xFFFFFF))
    draw(acc, sdf_convex([(62, 85), (78, 94), (62, 103)]), hexrgb(0xFFFFFF))


def icon_tiltmaze(acc):
    # Tilt game: a little maze with a ball and a goal.
    bg(acc, 0x0B1020)
    cell, ox, oy = 12, 14, 14
    rows = [
        "WWWWWWW",
        "WO....W",
        "W.WWW.W",
        "W.W.W.W",
        "W...W.W",
        "W.WW.GW",
        "WWWWWWW",
    ]
    palette = {"W": 0x2E5BFF, "O": 0x12203A, "G": 0x12203A}
    draw_pixmap(acc, ox, oy, cell, rows, palette)
    # ball (yellow) and goal (green) drawn round on top of their cells
    draw(acc, sdf_circle(ox + 1 * cell + cell / 2.0, oy + 1 * cell + cell / 2.0, cell / 2.0 - 1),
         hexrgb(0xFFD93B))
    pxrect(acc, ox + 5 * cell + 2, oy + 5 * cell + 2, cell - 4, cell - 4, 0x4CD964)


def icon_pixeljump(acc):
    # Button game: a runner mid-jump over an obstacle, on a ground line.
    bg(acc, 0x12182B)
    cell, ox, oy = 8, 14, 10
    rows = [
        "............",
        "...HH.......",
        "..HHHH......",
        "...HH.......",
        "..H.H.......",
        "............",
        ".........C..",
        ".......C.C..",
        ".......CCC..",
        "GGGGGGGGGGGG",
        "............",
        "............",
    ]
    palette = {"H": 0xFFD93B, "C": 0x4CD964, "G": 0x5A6378}
    draw_pixmap(acc, ox, oy, cell, rows, palette, inset=0.5)


ICONS = [
    ("bullseye", icon_bullseye),
    ("tappop", icon_tappop),
    ("colormatch", icon_colormatch),
    ("doodle", icon_doodle),
    ("fruitninja", icon_fruitninja),
    ("voiceplane", icon_voiceplane),
    ("pixelreflex", icon_pixelreflex),
    ("pixelsnake", icon_pixelsnake),
    ("pixel2048", icon_pixel2048),
    ("tiltmaze", icon_tiltmaze),
    ("pixeljump", icon_pixeljump),
]


def emit_c(name, acc):
    # ARGB8888: bytes per pixel are B, G, R, A (little-endian 0xAARRGGBB).
    out = []
    for y in range(H):
        for x in range(W):
            r, g, b, a = acc[y][x]
            ai = int(round(a * 255))
            ri = int(round(r)); gi = int(round(g)); bi = int(round(b))
            ri = max(0, min(255, ri)); gi = max(0, min(255, gi)); bi = max(0, min(255, bi))
            out.append((bi, gi, ri, ai))

    lines = []
    lines.append("// Auto-generated by tools/gen_game_icons.py — do not edit by hand.")
    lines.append("#if defined(LV_LVGL_H_INCLUDE_SIMPLE)")
    lines.append('#include "lvgl.h"')
    lines.append("#elif defined(LV_LVGL_H_INCLUDE_SYSTEM)")
    lines.append("#include <lvgl.h>")
    lines.append("#else")
    lines.append('#include "lvgl/lvgl.h"')
    lines.append("#endif")
    lines.append("")
    lines.append("#ifndef LV_ATTRIBUTE_MEM_ALIGN")
    lines.append("#define LV_ATTRIBUTE_MEM_ALIGN")
    lines.append("#endif")
    lines.append("")
    sym = "icon_%s" % name
    lines.append("static const LV_ATTRIBUTE_MEM_ALIGN uint8_t %s_map[] = {" % sym)
    row = []
    buf = []
    for i, (b, g, r, a) in enumerate(out):
        row.append("0x%02x,0x%02x,0x%02x,0x%02x," % (b, g, r, a))
        if len(row) >= 8:
            buf.append("    " + "".join(row))
            row = []
    if row:
        buf.append("    " + "".join(row))
    lines.append("\n".join(buf))
    lines.append("};")
    lines.append("")
    lines.append("const lv_image_dsc_t %s = {" % sym)
    lines.append("  .header = {")
    lines.append("    .magic = LV_IMAGE_HEADER_MAGIC,")
    lines.append("    .cf = LV_COLOR_FORMAT_ARGB8888,")
    lines.append("    .flags = 0,")
    lines.append("    .w = %d," % W)
    lines.append("    .h = %d," % H)
    lines.append("    .stride = %d," % (W * 4))
    lines.append("    .reserved_2 = 0,")
    lines.append("  },")
    lines.append("  .data_size = sizeof(%s_map)," % sym)
    lines.append("  .data = %s_map," % sym)
    lines.append("  .reserved = NULL,")
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def main():
    os.makedirs(OUTDIR, exist_ok=True)
    for name, fn in ICONS:
        acc = new_acc()
        fn(acc)
        path = os.path.join(OUTDIR, "icon_%s.c" % name)
        with open(path, "w") as fh:
            fh.write(emit_c(name, acc))
        print("wrote", path)

    header = ["#pragma once", "", '#include "lvgl.h"', ""]
    for name, _ in ICONS:
        header.append("LV_IMAGE_DECLARE(icon_%s);" % name)
    header.append("")
    with open(os.path.join(OUTDIR, "game_icons.h"), "w") as fh:
        fh.write("\n".join(header))
    print("wrote game_icons.h")


if __name__ == "__main__":
    main()
