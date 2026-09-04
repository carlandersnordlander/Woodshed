# Generates one SVG mockup per view, at the sizes and colours Theme.cpp actually uses.
#
# A generator rather than six hand-written files: the header, the palette and the metrics appear in
# every view, and six copies of them by hand is six chances for them to drift apart. Change a value
# here and every mockup follows.
#
#     python design/make_view_mockups.py
#
# The output opens directly in Illustrator and imports into Figma.

import os

W, H = 1440, 900
HEADER_H = 118
FOOTER_H = 40

# --- the palette, from standalone/ui/Theme.cpp -------------------------------------------------
BASE = "#0A0B0D"
SURFACE = "#131519"
RAISED = "#1F232A"
HOVER = "#2B3039"
SUNKEN = "#07080A"
LINE = "#262A32"
TEXT = "#E6E8EC"
DIM = "#868C97"
FAINT = "#5A606B"
ACCENT = "#F0A03E"
SUCCESS = "#6CC684"
DANGER = "#E86A6A"
CONTROL = "#ECECF0"

FONT = "Segoe UI, Inter, system-ui, sans-serif"
ROW = 25          # frame height
R_PANEL = 8
R_CTRL = 5


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def rect(x, y, w, h, fill, r=0, opacity=None, stroke=None, sw=1):
    o = f' opacity="{opacity}"' if opacity is not None else ""
    s = f' stroke="{stroke}" stroke-width="{sw}"' if stroke else ""
    return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{r}" fill="{fill}"{o}{s}/>'


def text(x, y, s, fill=TEXT, size=13, weight=None, anchor=None, ls=None):
    w = f' font-weight="{weight}"' if weight else ""
    a = f' text-anchor="{anchor}"' if anchor else ""
    l = f' letter-spacing="{ls}"' if ls else ""
    return f'<text x="{x}" y="{y}" fill="{fill}" font-size="{size}"{w}{a}{l}>{esc(s)}</text>'


def line(x1, y1, x2, y2, stroke=LINE, sw=1):
    return f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{stroke}" stroke-width="{sw}"/>'


def panel(x, y, w, h):
    return rect(x, y, w, h, SURFACE, R_PANEL)


def button(x, y, label, w=96, active=False, primary=False):
    out = []
    if active:
        out.append(rect(x, y, w, ROW, ACCENT, R_CTRL, opacity=0.22))
        out.append(rect(x, y, w, ROW, "none", R_CTRL, stroke=ACCENT))
        out.append(text(x + w / 2, y + 17, label, ACCENT, 13, anchor="middle"))
    else:
        out.append(rect(x, y, w, ROW, RAISED, R_CTRL))
        out.append(text(x + w / 2, y + 17, label, TEXT if primary else DIM, 13, anchor="middle"))
    return "".join(out)


def field(x, y, w, value, placeholder=False):
    return rect(x, y, w, ROW, SUNKEN, R_CTRL) + text(x + 9, y + 17, value, FAINT if placeholder else TEXT)


def slider(x, y, w, fill=0.6, label=None):
    out = [rect(x, y + 9, w, 7, SUNKEN, 4)]
    out.append(rect(x, y + 9, int(w * fill), 7, ACCENT, 4, opacity=0.75))
    out.append(f'<circle cx="{x + int(w * fill)}" cy="{y + 12}" r="6" fill="{ACCENT}"/>')
    if label:
        out.append(text(x + w + 10, y + 17, label, DIM, 12))
    return "".join(out)


def section(x, y, label):
    return text(x, y, label.upper(), FAINT, 12.3, weight="600", ls="1.2")


def label(x, y, s):
    return text(x, y, s, DIM)


def knob(cx, cy, name, value, r=26, fill=0.7):
    out = [f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="{SUNKEN}"/>']
    out.append(f'<path d="M {cx - r * 0.71:.1f} {cy + r * 0.71:.1f} A {r} {r} 0 1 1 {cx + r * 0.71:.1f} '
               f'{cy + r * 0.71:.1f}" fill="none" stroke="{LINE}" stroke-width="4" stroke-linecap="round"/>')
    import math
    a0 = math.radians(135)
    a1 = a0 + math.radians(270) * fill
    large = 1 if fill > 0.5 else 0
    out.append(f'<path d="M {cx + r * math.cos(a0):.1f} {cy - r * math.sin(a0):.1f} A {r} {r} 0 {large} 1 '
               f'{cx + r * math.cos(a1):.1f} {cy - r * math.sin(a1):.1f}" fill="none" stroke="{ACCENT}" '
               f'stroke-width="4" stroke-linecap="round"/>')
    px, py = cx + r * 0.62 * math.cos(a1), cy - r * 0.62 * math.sin(a1)
    out.append(f'<line x1="{cx}" y1="{cy}" x2="{px:.1f}" y2="{py:.1f}" stroke="{CONTROL}" stroke-width="2.4" '
               f'stroke-linecap="round"/>')
    out.append(text(cx, cy + r + 18, name, DIM, 12, anchor="middle"))
    out.append(text(cx, cy + r + 34, value, TEXT, 13, weight="600", anchor="middle"))
    return "".join(out)


def vmeter(x, y, w, h, fill=0.55):
    out = [rect(x, y, w, h, SUNKEN, 3)]
    used = int(h * fill)
    out.append(rect(x, y + h - used, w, used, SUCCESS, 3, opacity=0.85))
    return "".join(out)


def nav_icon(x, y, size, kind, active):
    """The five destinations plus the gear, as the app draws them."""
    out = []
    if active:
        i = size * 0.06
        out.append(rect(x + i, y + i, size - 2 * i, size - 2 * i, ACCENT, size * 0.26, opacity=0.22))
    c = ACCENT if active else DIM
    cx, cy, r = x + size / 2, y + size / 2, size * 0.28
    g = f'stroke="{c}" stroke-width="2.4" fill="none" stroke-linecap="round" stroke-linejoin="round"'
    if kind == "rig":
        out.append(f'<g {g}><rect x="{cx - r * 0.88:.1f}" y="{cy - r:.1f}" width="{r * 1.76:.1f}" '
                   f'height="{r * 0.54:.1f}" rx="2"/><rect x="{cx - r * 0.8:.1f}" y="{cy - r * 0.22:.1f}" '
                   f'width="{r * 1.6:.1f}" height="{r * 1.22:.1f}" rx="2"/>'
                   f'<circle cx="{cx:.1f}" cy="{cy + r * 0.4:.1f}" r="{r * 0.34:.1f}"/></g>')
    elif kind == "player":
        out.append(f'<g {g}><path d="M {cx - r * 0.5:.1f} {cy - r * 0.75:.1f} L {cx + r * 0.8:.1f} {cy:.1f} '
                   f'L {cx - r * 0.5:.1f} {cy + r * 0.75:.1f} Z"/></g>')
    elif kind == "tuner":
        out.append(f'<g {g}><path d="M {cx - r:.1f} {cy + r * 0.5:.1f} A {r} {r} 0 0 1 {cx + r:.1f} '
                   f'{cy + r * 0.5:.1f}"/><line x1="{cx:.1f}" y1="{cy + r * 0.5:.1f}" '
                   f'x2="{cx + r * 0.55:.1f}" y2="{cy - r * 0.45:.1f}"/></g>')
    elif kind == "metronome":
        out.append(f'<g {g}><path d="M {cx - r * 0.75:.1f} {cy + r:.1f} L {cx - r * 0.22:.1f} {cy - r:.1f} '
                   f'L {cx + r * 0.22:.1f} {cy - r:.1f} L {cx + r * 0.75:.1f} {cy + r:.1f} Z"/>'
                   f'<line x1="{cx + r * 0.35:.1f}" y1="{cy - r * 0.7:.1f}" x2="{cx - r * 0.3:.1f}" '
                   f'y2="{cy + r * 0.6:.1f}"/></g>')
    elif kind == "library":
        out.append(f'<g {g}><rect x="{cx - r:.1f}" y="{cy - r * 0.85:.1f}" width="{r * 0.5:.1f}" '
                   f'height="{r * 1.7:.1f}" rx="1"/><rect x="{cx - r * 0.35:.1f}" y="{cy - r * 0.85:.1f}" '
                   f'width="{r * 0.5:.1f}" height="{r * 1.7:.1f}" rx="1"/>'
                   f'<rect x="{cx + r * 0.35:.1f}" y="{cy - r * 0.7:.1f}" width="{r * 0.5:.1f}" '
                   f'height="{r * 1.55:.1f}" rx="1"/></g>')
    elif kind == "settings":
        out.append(f'<g {g}><circle cx="{cx:.1f}" cy="{cy:.1f}" r="{r * 0.42:.1f}"/>'
                   f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="{r * 0.95:.1f}" stroke-dasharray="4 3"/></g>')
    return "".join(out)


def header(active_view):
    """Input on the left, where you are in the middle, output on the right."""
    out = [rect(0, 0, W, HEADER_H, BASE)]

    # left: what is coming in
    out.append(vmeter(48, 22, 8, 68, 0.6))
    out.append(knob(102, 52, "IN", "+2.0"))
    out.append(f'<circle cx="160" cy="40" r="12" fill="none" stroke="{ACCENT}" stroke-width="2.2" '
               f'stroke-dasharray="48 20" transform="rotate(-115 160 40)"/>')
    out.append(line(160, 29, 160, 40, ACCENT, 2.2))
    out.append(button(144, 60, "PRE", 58, active=True))

    # centre: the destinations
    size, gap = 56, 34
    total = size * 5 + gap * 4
    x0 = (W - total) / 2
    for i, kind in enumerate(["rig", "player", "tuner", "metronome", "library"]):
        out.append(nav_icon(x0 + i * (size + gap), 20, size, kind, kind == active_view))
    out.append(nav_icon(x0 + total + gap, 32, 34, "settings", active_view == "settings"))

    # right: what is going out
    out.append(knob(W - 108, 52, "OUT", "-3.5", fill=0.42))
    out.append(vmeter(W - 56, 22, 8, 68, 0.45))

    out.append(line(0, HEADER_H, W, HEADER_H, LINE))
    return "".join(out)


def footer(chain="Noise gate  ›  NAM: 6505 Rhythm  ›  IR: V30 412  ›  EQ"):
    y = H - FOOTER_H
    return (rect(0, y, W, FOOTER_H, BASE) + line(0, y, W, y, LINE)
            + text(24, y + 25, chain, DIM, 12.5)
            + text(W - 24, y + 25, "48 kHz  ·  256  ·  ASIO", FAINT, 12.5, anchor="end"))


def document(name, active_view, body, note):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
            f'font-family="{FONT}">\n'
            f'<title>Anders Amp — {name}</title>\n'
            f'<desc>{esc(note)}</desc>\n'
            f'{rect(0, 0, W, H, BASE)}\n'
            f'<g id="header">{header(active_view)}</g>\n'
            f'<g id="view">{body}</g>\n'
            f'<g id="footer">{footer()}</g>\n'
            f'</svg>\n')


# ================================ the views ====================================================

def view_rig():
    o = []
    top = HEADER_H + 24
    o.append(section(48, top, "Signal chain"))

    # The rail: three lanes, the middle one the series path.
    rail_y = top + 30
    o.append(panel(36, rail_y, W - 72, 260))

    mid = rail_y + 130
    o.append(line(60, mid, W - 96, mid, LINE, 2))

    def block(x, y, kind, name, colour, w=150, h=74, selected=False):
        b = [rect(x, y, w, h, SURFACE if not selected else RAISED, R_CTRL,
                  stroke=ACCENT if selected else LINE, sw=2 if selected else 1)]
        b.append(rect(x, y, w, 3, colour, 0))
        b.append(text(x + w / 2, y + 30, kind, colour, 12, weight="600", anchor="middle", ls="1"))
        b.append(text(x + w / 2, y + 52, name, TEXT, 13, anchor="middle"))
        return "".join(b)

    o.append(block(70, mid - 37, "NAM", "6505 Rhythm", ACCENT, selected=True))
    o.append(block(260, mid - 37, "CUT", "80 Hz — 8 kHz", "#6BA9B2"))

    # A parallel section: the line leaves and comes back.
    sx, ex = 440, 800
    upper, lower = mid - 96, mid + 96
    for yy in (upper, lower):
        o.append(f'<path d="M {sx} {mid} C {sx + 30} {mid} {sx + 30} {yy} {sx + 60} {yy} '
                 f'L {ex - 60} {yy} C {ex - 30} {yy} {ex - 30} {mid} {ex} {mid}" fill="none" '
                 f'stroke="{LINE}" stroke-width="2"/>')
    o.append(f'<circle cx="{sx}" cy="{mid}" r="7" fill="{SURFACE}" stroke="{ACCENT}" stroke-width="2"/>')
    o.append(f'<circle cx="{ex}" cy="{mid}" r="7" fill="{SURFACE}" stroke="{ACCENT}" stroke-width="2"/>')
    o.append(text(sx, mid + 26, "split", FAINT, 11, anchor="middle"))
    o.append(text(ex, mid + 26, "merge", FAINT, 11, anchor="middle"))
    o.append(block(520, upper - 37, "IR", "V30 412", SUCCESS))
    o.append(block(520, lower - 37, "COMP", "LA-2A", "#B27FB2"))

    o.append(block(840, mid - 37, "EQ", "5 band", "#9E7FB2"))

    o.append(rect(1010, mid - 22, 44, 44, RAISED, R_CTRL))
    o.append(text(1032, mid + 7, "+", DIM, 22, anchor="middle"))

    o.append(text(60, rail_y + 22, "Drag a block to move it. Hover shows its power and remove.",
                  FAINT, 12))

    # The selected block's panel.
    py = rail_y + 284
    o.append(panel(36, py, 700, 250))
    o.append(section(60, py + 30, "NAM · 6505 Rhythm"))
    o.append(knob(120, py + 100, "GAIN", "+4.5"))
    o.append(knob(230, py + 100, "BLEND", "0%", fill=0.0))
    o.append(knob(340, py + 100, "LEVEL", "-2.0", fill=0.45))
    o.append(label(440, py + 70, "Tone stack"))
    o.append(button(440, py + 82, "POST", 70, active=True))
    o.append(knob(560, py + 100, "LOW", "0.0", fill=0.5))
    o.append(knob(650, py + 100, "MID", "+2.0", fill=0.6))
    o.append(text(60, py + 210, "Remove block", DANGER, 13))

    o.append(panel(752, py, W - 788, 250))
    o.append(section(776, py + 30, "Capture"))
    o.append(field(776, py + 44, 300, "Search the library"))
    for i, n in enumerate(["6505 Rhythm", "JCM800 Crunch", "Twin Clean", "Recto Lead"]):
        yy = py + 82 + i * 30
        sel = i == 0
        if sel:
            o.append(rect(776, yy - 3, W - 836, 26, ACCENT, R_CTRL, opacity=0.22))
        o.append(text(788, yy + 14, n, TEXT if sel else DIM))
    return "".join(o)


def view_player():
    o = []
    y = HEADER_H + 18

    # project bar
    o.append(text(48, y + 16, "PROJECT", FAINT, 12.3, weight="600", ls="1.2"))
    o.append(text(120, y + 16, "Wish You Were Here", TEXT, 15, weight="600"))
    o.append(button(340, y, "Save", 80))
    o.append(button(428, y, "Save as…", 110))
    o.append(button(546, y, "Open project…", 150))
    o.append(line(36, y + 40, W - 36, y + 40, LINE))

    # toolbar
    y += 56
    o.append(button(48, y, "Open file…", 140, primary=True))
    o.append(button(196, y, "Close all", 110))
    o.append(button(314, y, "Separate stems…", 160))
    o.append(label(498, y + 17, "Level"))
    o.append(slider(548, y, 150, 0.7))
    o.append(label(720, y + 17, "Speed"))
    o.append(slider(770, y, 170, 0.5))
    o.append(button(950, y, "75%", 48))
    o.append(button(1004, y, "50%", 48))
    o.append(button(1058, y, "100%", 52))
    o.append(label(1128, y + 17, "Pitch"))
    o.append(slider(1172, y, 130, 0.5))

    # tempo row
    y += 40
    o.append(button(48, y, "Detect tempo", 140))
    o.append(label(204, y + 17, "BPM"))
    o.append(field(240, y, 90, "125.48"))
    o.append(button(336, y, "/2", 32))
    o.append(button(372, y, "x2", 32))
    o.append(label(418, y + 17, "Offset"))
    o.append(field(466, y, 110, "0.412 s"))
    o.append(field(586, y, 70, "4/bar"))
    o.append(button(668, y, "Fit", 52))
    o.append(label(732, y + 17, "Rows"))
    o.append(button(776, y, "–", 26))
    o.append(button(806, y, "+", 26))
    o.append(text(852, y + 17, "☑ Follow   ☑ Grid   ☑ Snap   ☑ Notes", DIM))
    o.append(text(1110, y + 17, "82% sure", DIM))
    o.append(text(1180, y + 17, "| follows the song, 118–132", FAINT, 12))

    # body
    y += 40
    body_h = 430
    track_w = 230
    o.append(panel(36, y, track_w, body_h))
    o.append(panel(36 + track_w + 8, y, W - 36 - track_w - 8 - 36 - 16, body_h))

    tl_x = 36 + track_w + 8
    tl_w = W - 36 - track_w - 8 - 36 - 16

    # ruler, pinned
    o.append(rect(tl_x, y, tl_w, 34, "#FFFFFF", 0, opacity=0.05))
    o.append(line(tl_x, y + 34, tl_x + tl_w, y + 34, LINE))
    for i in range(9):
        bx = tl_x + 20 + i * 118
        o.append(line(bx, y + 2, bx, y + 16, DIM))
        o.append(text(bx + 4, y + 13, str(i * 4 + 1), FAINT, 11))
    o.append(rect(tl_x + 138, y + 17, 236, 15, ACCENT, 2, opacity=0.95))
    o.append(text(tl_x + 144, y + 29, "Solo", "#0A0B0D", 12))

    # lanes
    lanes = [("Drums", 0.9), ("Bass", 0.7), ("Guitar", 0.6), ("Vocals", 0.5)]
    lane_h = (body_h - 34) / len(lanes)
    for i, (nm, amp) in enumerate(lanes):
        ly = y + 34 + i * lane_h
        if i:
            o.append(line(36, ly, tl_x + tl_w, ly, "#FFFFFF", 0.06))
        # controls
        o.append(text(52, ly + 22, nm, TEXT))
        o.append(button(track_w - 62, ly + 6, "N", 30))
        o.append(button(track_w - 28, ly + 6, "EQ", 30))
        o.append(text(52, ly + 48, "M   S", DIM, 12))
        o.append(slider(96, ly + 34, 100, 0.72))
        o.append(text(track_w + 14, ly + 48, "✕", FAINT, 13))
        # waveform
        import random
        random.seed(i * 7 + 3)
        mid_y = ly + lane_h * 0.38
        # Grouped and named, so the whole waveform is one object to select, hide or replace in a
        # drawing program rather than a few hundred loose strokes.
        pts = [f'<g id="waveform-{nm.lower()}" stroke="{ACCENT}" stroke-width="2" opacity="0.8">']
        for c in range(0, int(tl_w), 4):
            v = amp * (0.25 + 0.75 * abs(random.random() - 0.2)) * lane_h * 0.26
            pts.append(f'<line x1="{tl_x + c}" y1="{mid_y - v:.0f}" x2="{tl_x + c}" y2="{mid_y + v:.0f}"/>')
        pts.append("</g>")
        o.append("".join(pts))
        # chord ribbon on the first lane
        if i == 0:
            cy = ly + lane_h - 26
            for k, ch in enumerate(["Em", "Cmaj7", "G", "D5"]):
                cx = tl_x + 6 + k * 236
                o.append(rect(cx, cy, 230, 22, ACCENT, 3, opacity=0.22))
                o.append(line(cx, cy, cx, cy + 22, ACCENT, 1.5))
                o.append(text(cx + 7, cy + 16, ch, TEXT, 12.5))

    # playhead
    px = tl_x + 420
    o.append(line(px, y, px, y + body_h, CONTROL, 2))
    o.append(f'<path d="M {px - 5} {y} L {px + 5} {y} L {px} {y + 7} Z" fill="{CONTROL}"/>')

    # scrollbars
    o.append(rect(W - 48, y + 34, 11, body_h - 34, "#FFFFFF", 6, opacity=0.04))
    o.append(rect(W - 46, y + 40, 7, 150, "#FFFFFF", 4, opacity=0.18))
    o.append(rect(tl_x, y + body_h + 6, tl_w, 11, "#FFFFFF", 6, opacity=0.04))
    o.append(rect(tl_x + 40, y + body_h + 8, 320, 7, "#FFFFFF", 4, opacity=0.18))

    # transport
    ty = y + body_h + 32
    for i, (lb, w) in enumerate([("|◀", 46), ("◀◀", 46)]):
        o.append(button(48 + i * 50, ty, lb, w))
    o.append(button(148, ty, "PLAY", 96, active=True))
    o.append(button(248, ty, "STOP", 64))
    o.append(button(316, ty, "▶▶", 46))
    o.append(button(378, ty, "CLICK", 80, active=True))
    o.append(text(478, ty + 18, "1:24", TEXT, 18, weight="600"))
    o.append(text(524, ty + 18, "/ 5:38", DIM))
    o.append(button(596, ty, "◀", 36))
    o.append(text(646, ty + 17, "Solo", ACCENT))
    o.append(button(690, ty, "▶", 36))
    o.append(button(736, ty, "Free", 64))
    o.append(text(816, ty + 17, "keys", FAINT, 12))
    return "".join(o)


def view_tuner():
    o = []
    y = HEADER_H + 40
    o.append(panel(180, y, W - 360, 560))

    o.append(text(W / 2, y + 120, "A", TEXT, 130, weight="600", anchor="middle"))
    o.append(text(W / 2 + 78, y + 120, "2", DIM, 54, anchor="middle"))
    o.append(text(W / 2, y + 160, "110.00 Hz", DIM, 16, anchor="middle"))

    # needle
    cx, cy, r = W / 2, y + 380, 200
    o.append(f'<path d="M {cx - r} {cy} A {r} {r} 0 0 1 {cx + r} {cy}" fill="none" stroke="{LINE}" stroke-width="3"/>')
    for i in range(-5, 6):
        import math
        a = math.radians(180 - (i + 5) * 18)
        x1, y1 = cx + (r - 14) * math.cos(a), cy - (r - 14) * math.sin(a)
        x2, y2 = cx + r * math.cos(a), cy - r * math.sin(a)
        o.append(line(f"{x1:.1f}", f"{y1:.1f}", f"{x2:.1f}", f"{y2:.1f}", FAINT if i else SUCCESS, 3 if not i else 1.5))
    o.append(text(cx - r + 6, cy + 24, "−50", FAINT, 12))
    o.append(text(cx + r - 24, cy + 24, "+50", FAINT, 12))
    o.append(f'<line x1="{cx}" y1="{cy}" x2="{cx + 26}" y2="{cy - r + 30}" stroke="{SUCCESS}" '
             f'stroke-width="3.5" stroke-linecap="round"/>')
    o.append(f'<circle cx="{cx}" cy="{cy}" r="9" fill="{SUCCESS}"/>')

    # strings
    sy = y + 470
    notes = ["E1", "A1", "D2", "G2", "B2", "E3"]
    total = len(notes) * 74
    for i, n in enumerate(notes):
        bx = cx - total / 2 + i * 74
        o.append(button(bx, sy, n, 64, active=(i == 1)))
    o.append(text(cx, sy + 52, "Left and right arrows step between the strings", FAINT, 12, anchor="middle"))

    # settings strip
    o.append(section(210, y + 34, "Tuning"))
    o.append(field(210, y + 48, 160, "Guitar · 6 · Standard"))
    o.append(label(210, y + 100, "A4"))
    o.append(field(250, y + 88, 90, "440.0 Hz"))
    o.append(button(210, y + 128, "Needle", 90, active=True))
    o.append(button(308, y + 128, "Strobe", 90))
    o.append(text(210, y + 186, "☑ Mute output while tuning", DIM))
    return "".join(o)


def view_metronome():
    o = []
    y = HEADER_H + 40
    o.append(panel(180, y, 620, 560))
    o.append(panel(820, y, W - 1000, 560))

    o.append(text(490, y + 130, "120", TEXT, 96, weight="600", anchor="middle"))
    o.append(text(490, y + 162, "BPM", DIM, 15, anchor="middle"))
    o.append(button(300, y + 190, "–", 40))
    o.append(field(350, y + 190, 90, "120"))
    o.append(button(450, y + 190, "+", 40))
    o.append(button(510, y + 190, "Tap", 90))

    # pendulum
    import math
    px, py = 490, y + 300
    o.append(f'<path d="M {px - 90} {py + 170} L {px - 26} {py} L {px + 26} {py} L {px + 90} {py + 170} Z" '
             f'fill="none" stroke="{LINE}" stroke-width="2"/>')
    a = math.radians(24)
    o.append(f'<line x1="{px}" y1="{py + 165}" x2="{px + 150 * math.sin(a):.0f}" '
             f'y2="{py + 165 - 150 * math.cos(a):.0f}" stroke="{ACCENT}" stroke-width="4" stroke-linecap="round"/>')
    o.append(f'<circle cx="{px + 100 * math.sin(a):.0f}" cy="{py + 165 - 100 * math.cos(a):.0f}" r="12" '
             f'fill="{ACCENT}"/>')

    # beats
    by = y + 500
    for i in range(4):
        bx = 490 - 110 + i * 74
        on = i == 0
        o.append(f'<circle cx="{bx}" cy="{by}" r="15" fill="{ACCENT if on else RAISED}"/>')
        o.append(text(bx, by + 5, str(i + 1), "#0A0B0D" if on else DIM, 13, anchor="middle"))

    x = 850
    o.append(section(x, y + 34, "Time signature"))
    o.append(field(x, y + 48, 70, "4"))
    o.append(text(x + 80, y + 65, "/", DIM, 17))
    o.append(field(x + 96, y + 48, 70, "4"))
    o.append(label(x, y + 108, "Subdivision"))
    o.append(field(x, y + 118, 166, "Eighths"))

    o.append(section(x, y + 186, "Sounds"))
    for i, (nm, snd) in enumerate([("Accent", "Beep"), ("Beat", "Beep"), ("Sub", "Tick")]):
        yy = y + 204 + i * 46
        o.append(label(x, yy + 17, nm))
        o.append(field(x + 70, yy, 96, snd))
        o.append(slider(x + 176, yy, 110, 0.6 - i * 0.15))

    o.append(section(x, y + 372, "Presets"))
    for i, nm in enumerate(["Ballad 72", "Shuffle 96", "Practice 120"]):
        o.append(button(x, y + 390 + i * 32, nm, 200))

    o.append(button(x, y + 500, "START", 200, active=True))
    return "".join(o)


def view_library():
    o = []
    y = HEADER_H + 18
    # source tabs
    for i, nm in enumerate(["Rigs", "All", "Favourites", "Recent", "TONE3000"]):
        bx = 48 + i * 118
        act = i == 1
        if act:
            o.append(rect(bx, y, 108, 30, RAISED, 6))
            o.append(rect(bx, y, 108, 2, ACCENT, 0))
        o.append(text(bx + 54, y + 20, nm, TEXT if act else DIM, 13, anchor="middle"))
    o.append(field(W - 360, y + 2, 312, "Search captures"))

    y += 48
    h = 560
    # filters
    o.append(panel(36, y, 280, h))
    o.append(section(58, y + 28, "Filters"))
    o.append(text(58, y + 48, "Expand all · Collapse all", FAINT, 12))
    for i, (nm, items) in enumerate([("Tags", ["clean", "crunch", "high gain"]),
                                     ("Gear", ["6505", "JCM800", "Twin"]),
                                     ("Creator", ["Anders", "Felix"])]):
        gy = y + 76 + i * 148
        o.append(text(58, gy, "▾ " + nm, TEXT, 13, weight="600"))
        o.append(field(58, gy + 10, 230, "Search " + nm.lower(), placeholder=True))
        for k, it in enumerate(items):
            o.append(text(74, gy + 56 + k * 24, ("☑ " if (i == 0 and k == 1) else "☐ ") + it, DIM))

    # list
    lx = 332
    lw = W - 332 - 36 - 320 - 8
    o.append(panel(lx, y, lw, h))
    cols = [("Name", 0), ("Type", 250), ("Gear", 330), ("Creator", 440), ("Added", 560)]
    for nm, dx in cols:
        o.append(text(lx + 20 + dx, y + 30, nm + (" ▾" if nm == "Name" else ""), FAINT, 12, weight="600"))
    o.append(line(lx + 12, y + 40, lx + lw - 12, y + 40, LINE))
    rows = [("6505 Rhythm", "NAM", "6505", "Anders", "2 d"),
            ("JCM800 Crunch", "NAM", "JCM800", "Felix", "5 d"),
            ("V30 412 close", "IR", "Mesa 412", "Anders", "1 w"),
            ("Twin Clean", "NAM", "Twin", "Felix", "2 w"),
            ("Recto Lead", "NAM", "Recto", "Anders", "3 w")]
    for i, r in enumerate(rows):
        ry = y + 52 + i * 34
        if i == 0:
            o.append(rect(lx + 12, ry - 4, lw - 24, 30, ACCENT, R_CTRL, opacity=0.22))
        for (nm, dx), val in zip(cols, r):
            o.append(text(lx + 20 + dx, ry + 16, val, TEXT if i == 0 else DIM))

    # details
    dx0 = W - 36 - 320
    o.append(panel(dx0, y, 320, h))
    o.append(section(dx0 + 22, y + 28, "6505 Rhythm"))
    o.append(text(dx0 + 22, y + 62, "NAM capture · 12.4 MB", DIM, 12))
    o.append(text(dx0 + 22, y + 96, "Tags", FAINT, 12, weight="600"))
    for i, t in enumerate(["crunch", "6505", "rhythm"]):
        o.append(rect(dx0 + 22 + i * 78, y + 106, 70, 20, ACCENT, 10, opacity=0.22))
        o.append(text(dx0 + 57 + i * 78, y + 120, t, ACCENT, 11, anchor="middle"))
    o.append(field(dx0 + 22, y + 138, 276, "Add a tag", placeholder=True))
    o.append(button(dx0 + 22, y + 186, "Load into block 1", 276, primary=True))
    o.append(button(dx0 + 22, y + 220, "Favourite", 134))
    o.append(button(dx0 + 164, y + 220, "Reveal", 134))
    o.append(text(dx0 + 22, y + 280, "Remove from disk", DANGER, 13))
    return "".join(o)


def view_settings():
    o = []
    y = HEADER_H + 24
    h = 600
    o.append(panel(36, y, 460, h))
    o.append(panel(512, y, W - 548, h))

    o.append(section(60, y + 30, "Audio device"))
    for i, (nm, val) in enumerate([("API", "ASIO"), ("Input", "Focusrite USB"), ("Output", "Focusrite USB"),
                                   ("Sample rate", "48000"), ("Buffer", "256")]):
        yy = y + 52 + i * 44
        o.append(label(60, yy + 17, nm))
        o.append(field(180, yy, 280, val))
    o.append(button(60, y + 292, "Apply", 120, primary=True))
    o.append(text(60, y + 344, "Running · 48 kHz · 256 frames · 5.3 ms", FAINT, 12))

    x = 540
    o.append(section(x, y + 30, "Levels"))
    o.append(text(x, y + 56, "☑ Match capture loudness", DIM))
    o.append(text(x, y + 82, "Captures carry the loudness they were trained at. With this on,",
                  FAINT, 12))
    o.append(text(x, y + 100, "switching capture keeps you equally loud.", FAINT, 12))

    o.append(section(x, y + 140, "Stem separation"))
    o.append(field(x, y + 156, W - 588, "demucs   (leave empty to find it automatically)", placeholder=True))
    o.append(text(x, y + 200, "Stems are written to %APPDATA%\\NAMStandaloneUI\\stems", FAINT, 12))

    o.append(section(x, y + 240, "Chord transcription"))
    o.append(field(x, y + 256, W - 588, "python   (leave empty to find it automatically)", placeholder=True))
    o.append(text(x, y + 300, "Using: …\\NAMStandaloneUI\\python311\\Scripts\\python.exe  (set up beside this app)",
                  FAINT, 12))

    o.append(section(x, y + 340, "Capture folder"))
    o.append(text(x, y + 366, "D:\\Captures", DIM, 12))
    o.append(button(x, y + 380, "Choose folder", 200))

    o.append(section(x, y + 440, "TONE3000"))
    o.append(text(x, y + 466, "Signed in as anders", SUCCESS, 13))
    o.append(button(x, y + 480, "Sign out", 140))
    return "".join(o)


VIEWS = [
    ("rig", "Rig", view_rig, "The chain: blocks left to right, a parallel section leaving the line and coming back, and the selected block's controls underneath."),
    ("player", "Player", view_player, "Project, transport, tracks and timeline. The ruler is pinned; the lanes scroll under it."),
    ("tuner", "Tuner", view_tuner, "Needle mode, with the strings of the chosen tuning as lock buttons."),
    ("metronome", "Metronome", view_metronome, "Tempo, sounds and presets, with the pendulum driven by the beat phase."),
    ("library", "Library", view_library, "Sources across the top, filters at the left, the list, and what is known about the selection."),
    ("settings", "Settings", view_settings, "The device on the left; everything that is not the device on the right."),
]


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out_dir = os.path.join(here, "views")
    os.makedirs(out_dir, exist_ok=True)

    for key, name, builder, note in VIEWS:
        svg = document(name, key, builder(), note)
        path = os.path.join(out_dir, f"{key}.svg")
        with open(path, "w", encoding="utf-8") as f:
            f.write(svg)
        print(f"wrote {os.path.relpath(path, here)}  ({len(svg)} bytes)")


if __name__ == "__main__":
    main()
