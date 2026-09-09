"""Draw the CDJ-2000 front panel, and put the LCD in the middle of it.

This is presentation only.  Every input still comes from
`tools.cdj_main.panel_control` and every click still goes out through
`view_ui.Control.lines`, so the measured half of the project -- which bit, which
name, which run -- is untouched.  What changes is that the window stops looking
like a debugger and starts looking like the deck in
`cdj2000-interface-real-unit.jpg`.

The approach is borrowed from `nsaintot/cdj3k-emu`, which is a CDJ-3000 (RK3399
+ Linux, aarch64) and shares no architecture with this project at all -- but its
UI crate answers a question we had not: it draws the chassis **procedurally**
into a reference coordinate system and scales that to the window
(`crates/cdj3k-emu-ui/src/app/ui/layout.rs`), rather than skinning a photo.  So
there is no artwork to keep in step with the code, and the window resizes
without resampling anything.  Its `bloom.rs` does the backlight as a blurred
copy of the lit shapes; `draw_cache.rs`/`frame_cache.rs` keep the static chrome
out of the per-frame path.  All three ideas are used below.

**Where a control sits here is not evidence of anything.**  That rule has cost
this project weeks twice -- the four SOURCE keys were labelled from where they
sit on the front panel and were reversed for as long as the project existed.  So
`PLACEMENTS` only positions an input when MAIN's own SERVICE MODE name table
names it (`panel_control.FIRMWARE_KEY_NAMES`), and the arrangement is drawn from
that name, not from a guess about the photo.  Inputs the firmware does not name
-- 15.6, 15.7 and seven of the eight analogue fields -- are **not** given a
plausible-looking spot on the deck.  They go in the rack along the bottom,
captioned as what they are.  A window that invents a TEMPO fader out of an
unattributed analogue field would be making exactly the claim this project
refuses to make.

The lit state is likewise honest: a key lights when *you* press it and fades,
because that is what the window knows.  It is not reading the panel's LED
lines -- nothing has ever measured those -- so it does not pretend to.
"""
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 LycheeAPPF

from __future__ import annotations

import math
import tkinter as tk
from typing import Callable, NamedTuple

from PIL import Image, ImageDraw, ImageFilter, ImageTk

# ------------------------------------------------------------------ palette --
#
# Taken from cdj3k-emu's `ui.rs` where it fits (that deck is the same industrial
# design language), and from the reference photo where it does not: the
# CDJ-2000's SOURCE column is blue for LINK and amber for the rest, and the
# browse row is amber on near-black.
CHASSIS = (18, 20, 24)
CHASSIS_EDGE = (8, 9, 12)
PLATE = (35, 38, 44)
BTN = (43, 47, 54)
BTN_EDGE = (92, 98, 110)
BTN_TEXT = (210, 214, 224)
DIM_TEXT = (137, 143, 156)
AMBER = (226, 190, 42)
AMBER_LIT = (255, 224, 84)
BLUE = (48, 133, 255)
BLUE_LIT = (124, 185, 255)
WHITE = (230, 234, 242)
CUE_ORANGE = (255, 153, 38)
PLAY_GREEN = (33, 201, 111)
RED = (244, 86, 101)
LCD_SURROUND = (5, 7, 10)

# ----------------------------------------------------------------- geometry --
#
# One panel unit is one LCD pixel, so the picture is never resampled: at
# `--scale 2` the 480x234 frame is 960x468 and every chrome coordinate below is
# simply doubled.  cdj3k-emu keeps a 4080-unit reference canvas for the same
# reason -- one coordinate system, scaled once at the edge.
PANEL_W = 672
PANEL_H = 486
RACK_H = 0                      # diagnostics live in the Inspector

LCD_X, LCD_Y = 86, 56
LCD_W, LCD_H = 480, 234


class Place(NamedTuple):
    """Where one input is drawn, and how it looks when it is lit."""
    x: int
    y: int
    w: int
    h: int
    label: str
    accent: tuple[int, int, int] = AMBER
    shape: str = "rect"         # rect | round | knob | wheel
    font: int = 9


# The deck, in panel units.  Only inputs MAIN's name table names appear here.
#
# The top zone reproduces the reference photo one-for-one, because that photo is
# the acceptance target (memory cdj-reference-photo-target): BROWSE / TAG LIST /
# INFO / MENU across the top, LINK / USB / SD / DISC down the left, the panel
# between them.  The lower zone is the rest of the deck in its real relative
# arrangement but compressed -- the LCD is the subject here, so it keeps its
# native pixels and the transport gives up room rather than the other way round.
PLACEMENTS: dict[str, Place] = {
    # ---- above the panel ----
    "20.0": Place(86, 18, 114, 30, "BROWSE", AMBER, font=11),
    "20.1": Place(208, 18, 114, 30, "TAG LIST", AMBER, font=11),
    "20.2": Place(330, 18, 114, 30, "INFO", AMBER, font=11),
    "20.3": Place(452, 18, 114, 30, "MENU", AMBER, font=11),
    # Not a key of its own on the player: UTILITY is MENU held down, and on
    # this link "held down" is a press spanning two of MAIN's 3 s status
    # records (view_ui.WINDOW_LONG_HOLD_MS).  The suffix keeps it out of the
    # bit count (coverage strips it, as it does field6-touch) and lets
    # view_ui resolve it to the long-press control.
    "20.3-hold": Place(586, 18, 76, 30, "UTILITY", AMBER, font=10),
    # ---- the SOURCE column, left of the panel ----
    "19.0": Place(8, 62, 70, 26, "LINK", BLUE, font=10),
    "19.1": Place(8, 96, 70, 26, "USB", AMBER, font=10),
    "19.2": Place(8, 130, 70, 26, "SD", AMBER, font=10),
    "19.3": Place(8, 164, 70, 26, "DISC", AMBER, font=10),
    "17.2": Place(8, 206, 70, 22, "SD LID", DIM_TEXT, font=8),
    "20.5": Place(8, 234, 70, 22, "TAG TRACK", AMBER, font=8),
    # ---- the selector, right of the panel ----
    "17.0": Place(586, 62, 76, 76, "PUSH", WHITE, shape="knob"),
    "20.4": Place(586, 148, 76, 24, "RETURN", AMBER, font=9),
    "21.3": Place(586, 178, 76, 22, "MEMORY", AMBER, font=8),
    "21.2": Place(586, 204, 76, 22, "DELETE", RED, font=8),
    "21.0": Place(586, 230, 36, 22, "< CALL", AMBER, font=7),
    "21.1": Place(626, 230, 36, 22, "CALL >", AMBER, font=7),
    "19.4": Place(586, 256, 76, 22, "TIME/A.CUE", DIM_TEXT, font=7),
    # ---- the jog and its ring, lower left ----
    "15.5": Place(30, 306, 172, 172, "JOG", WHITE, shape="wheel"),
    "18.6": Place(214, 306, 58, 24, "JOG MODE", DIM_TEXT, font=7),
    "15.1": Place(214, 334, 58, 24, "REV", DIM_TEXT, font=8),
    "15.0": Place(214, 362, 58, 24, "LOCK", DIM_TEXT, font=8),
    # ---- transport ----
    "18.1": Place(214, 396, 58, 24, "|<< PREV", DIM_TEXT, font=7),
    "18.2": Place(214, 424, 58, 24, "NEXT >>|", DIM_TEXT, font=7),
    "18.3": Place(214, 452, 28, 24, "<<", DIM_TEXT, font=8),
    "18.4": Place(244, 452, 28, 24, ">>", DIM_TEXT, font=8),
    "16.1": Place(30, 486 - 0, 0, 0, "", CUE_ORANGE),   # replaced below
    # ---- hot cue / loop / rec, the row above the tempo side ----
    "16.5": Place(288, 306, 56, 34, "HOT CUE A", AMBER, font=7),
    "16.6": Place(350, 306, 56, 34, "HOT CUE B", AMBER, font=7),
    "16.7": Place(412, 306, 56, 34, "HOT CUE C", AMBER, font=7),
    "18.0": Place(474, 306, 56, 34, "REC MODE", RED, font=7),
    "16.4": Place(288, 348, 56, 28, "LOOP IN", AMBER, font=7),
    "16.3": Place(350, 348, 56, 28, "LOOP OUT", AMBER, font=7),
    "16.2": Place(412, 348, 56, 28, "RELOOP", AMBER, font=7),
    "17.1": Place(474, 348, 56, 28, "4-BEAT", AMBER, font=7),
    # ---- tempo, right of the deck ----
    "18.7": Place(586, 306, 76, 24, "TEMPO RANGE", DIM_TEXT, font=7),
    "19.6": Place(586, 334, 76, 24, "MASTER TEMPO", AMBER, font=7),
    "19.7": Place(586, 362, 76, 24, "TEMPO RESET", DIM_TEXT, font=7),
}

# CUE and PLAY are the two big ones and get written out rather than squeezed
# into the table above, because their size is the point: on the deck they are
# the only controls you find without looking.
PLACEMENTS["16.1"] = Place(288, 396, 108, 52, "CUE", CUE_ORANGE, font=13)
PLACEMENTS["16.0"] = Place(404, 396, 108, 52, "PLAY/PAUSE", PLAY_GREEN, font=11)

# A centered platter with transport to its left: a deck, not a button matrix.
# Input IDs stay unchanged; this is presentation only.
PLACEMENTS.update({
    "15.5": Place(242, 302, 178, 178, "JOG", WHITE, shape="wheel"),
    "16.1": Place(28, 348, 60, 60, "CUE", CUE_ORANGE, shape="round", font=13),
    "16.0": Place(28, 418, 60, 60, "PLAY / II", PLAY_GREEN, shape="round", font=9),
    "18.1": Place(108, 350, 56, 24, "|<< PREV", DIM_TEXT, font=7),
    "18.2": Place(172, 350, 56, 24, "NEXT >>|", DIM_TEXT, font=7),
    "18.3": Place(108, 384, 56, 24, "<<", DIM_TEXT),
    "18.4": Place(172, 384, 56, 24, ">>", DIM_TEXT),
    "18.6": Place(108, 426, 120, 24, "JOG MODE", DIM_TEXT),
    "15.1": Place(108, 456, 56, 22, "REV", DIM_TEXT),
    "15.0": Place(172, 456, 56, 22, "LOCK", DIM_TEXT),
    "16.5": Place(24, 306, 46, 28, "CUE A", AMBER, font=7),
    "16.6": Place(76, 306, 46, 28, "CUE B", AMBER, font=7),
    "16.7": Place(128, 306, 46, 28, "CUE C", AMBER, font=7),
    "18.0": Place(180, 306, 48, 28, "REC", RED, font=8),
    "16.4": Place(444, 306, 56, 28, "LOOP IN", AMBER, font=7),
    "16.3": Place(508, 306, 56, 28, "LOOP OUT", AMBER, font=7),
    "16.2": Place(444, 344, 56, 28, "RELOOP", AMBER, font=7),
    "17.1": Place(508, 344, 56, 28, "4-BEAT", AMBER, font=7),
})


def fit_scale(width: int, height: int, maximum: float = 3.0) -> float:
    """Fit the entire deck, with a little breathing room, into the viewport."""
    return max(0.1, min(maximum, (width - 24) / PANEL_W,
                        (height - 24) / (PANEL_H + RACK_H)))

# The SELECT encoder is the same physical control as 17.0 -- the knob turns and
# pushes -- so it is drawn once and carries both.
ENCODER_FIELD_PLACE = "17.0"


def lerp(a: tuple[int, int, int], b: tuple[int, int, int],
         t: float) -> tuple[int, int, int]:
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def _font(size: int):
    """A bitmap font at `size`, or PIL's default.

    Deliberately forgiving: a missing DejaVu on someone else's machine must
    give a plainer window, not a traceback in a launcher that has already
    started two emulators.
    """
    from PIL import ImageFont
    for name in ("DejaVuSans-Bold.ttf", "arialbd.ttf", "seguisb.ttf",
                 "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
                 "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    try:
        return ImageFont.load_default(size=size)
    except TypeError:  # Pillow 9 still supports the project's minimum version.
        return ImageFont.load_default()


class Renderer:
    """Turns `Place`s into images: the chassis once, each key in two states."""

    def __init__(self, scale: int) -> None:
        self.scale = scale
        self.fonts: dict[int, object] = {}

    def font(self, size: int):
        key = max(7, int(size * self.scale * 0.92))
        if key not in self.fonts:
            self.fonts[key] = _font(key)
        return self.fonts[key]

    # -- the static plate -------------------------------------------------
    def chassis(self) -> Image.Image:
        """Everything that never changes: plate, LCD surround, engraving.

        Drawn once at startup.  cdj3k-emu keeps the same split (`draw_cache`
        holds the static shapes; only the LCD texture and the pressed keys are
        rebuilt per frame), and it is the difference between a window that
        repaints 480x234 pixels and one that repaints the whole deck.
        """
        s = self.scale
        size = (PANEL_W * s, (PANEL_H + RACK_H) * s)
        image = Image.new("RGB", size, CHASSIS)
        draw = ImageDraw.Draw(image)

        # A shallow vertical gradient so the plate reads as metal rather than
        # as a flat fill.  Cheap: one line per row of the deck.
        for y in range(PANEL_H * s):
            t = y / max(1, PANEL_H * s - 1)
            # A narrow highlight at the top and a darker foot make the panel
            # feel like a shallow piece of hardware instead of a grey bitmap.
            tone = lerp(PLATE, CHASSIS, min(1.0, t * 0.92))
            if y < 2 * s:
                tone = lerp(tone, (86, 90, 100), 0.28)
            draw.line([(0, y), (size[0], y)], fill=tone)
        draw.rectangle([0, PANEL_H * s, size[0], size[1]], fill=CHASSIS_EDGE)

        # Recessed working zones keep the dense lower panel legible while
        # staying quiet enough that the firmware LCD remains the focal point.
        zones = [
            (5, 52, 78, 288),       # source column
            (16, 298, 234, 482),    # hot cues / transport
            (436, 298, 572, 382),   # loop
            (578, 298, 668, 390),   # tempo controls
        ]
        for x1, y1, x2, y2 in zones:
            box = [x1 * s, y1 * s, x2 * s, y2 * s]
            draw.rounded_rectangle(box, radius=5 * s,
                                   fill=lerp(CHASSIS, PLATE, 0.18),
                                   outline=(48, 52, 60),
                                   width=max(1, s // 2))

        # The LCD's bezel: the panel is inset in the real deck, so a dark
        # surround with a light top edge.
        bezel = [(LCD_X - 6) * s, (LCD_Y - 6) * s,
                 (LCD_X + LCD_W + 6) * s, (LCD_Y + LCD_H + 6) * s]
        draw.rounded_rectangle(bezel, radius=5 * s, fill=LCD_SURROUND,
                               outline=(72, 78, 90), width=max(1, s))
        draw.line([(LCD_X * s, (LCD_Y - 3) * s),
                   ((LCD_X + LCD_W) * s, (LCD_Y - 3) * s)],
                  fill=(103, 110, 123), width=max(1, s // 2))

        # The jog well, so the wheel does not float on the plate.
        jog = PLACEMENTS["15.5"]
        cx, cy = (jog.x + jog.w / 2) * s, (jog.y + jog.h / 2) * s
        r = jog.w / 2 * s
        draw.ellipse([cx - r - 6 * s, cy - r - 6 * s,
                      cx + r + 6 * s, cy + r + 6 * s],
                     fill=(18, 18, 22), outline=(58, 58, 66),
                     width=max(1, s // 2))

        # Engraved labels are presentation, not mappings: they name only the
        # already-measured groups whose controls are placed below.
        engravings = [
            ((10, 53), "SOURCE"),
            ((28, 299), "PERFORMANCE"),
            ((446, 299), "LOOP"),
            ((586, 299), "TEMPO"),
        ]
        for (x, y), text in engravings:
            draw.text((x * s, y * s), text, font=self.font(6),
                      fill=(105, 111, 124))

        draw.text((588 * s, 283 * s), "CDJ–2000", font=self.font(10),
                  fill=(207, 211, 220))

        # Four restrained fasteners finish the instrument-panel silhouette.
        for x, y in ((8, 8), (664, 8), (8, 478), (664, 478)):
            rr = 3 * s
            draw.ellipse([x * s - rr, y * s - rr,
                          x * s + rr, y * s + rr],
                         fill=(24, 26, 31), outline=(65, 69, 78),
                         width=max(1, s // 2))
            draw.line([(x * s - 1.5 * s, y * s),
                       (x * s + 1.5 * s, y * s)], fill=(91, 96, 107),
                      width=max(1, s // 2))

        return image

    # -- one key, unlit and lit -------------------------------------------
    def key(self, place: Place, lit: bool,
            hovered: bool = False) -> Image.Image:
        """A single key with its own glow, on transparent background.

        Returned oversized by `pad` on each side so the bloom has somewhere to
        go; the caller places it centred on the key's rectangle.
        """
        s = self.scale
        pad = 10 * s
        w, h = place.w * s, place.h * s
        image = Image.new("RGBA", (w + 2 * pad, h + 2 * pad), (0, 0, 0, 0))

        if place.shape == "knob":
            self._knob(image, place, lit, pad, hovered)
        elif place.shape == "wheel":
            self._wheel(image, place, lit, pad, hovered)
        else:
            self._rect(image, place, lit, pad, hovered)
        return image

    def _bloom(self, image: Image.Image, shape: Image.Image,
               accent: tuple[int, int, int], strength: float) -> None:
        """cdj3k-emu's `bloom.rs`, in two lines of Pillow.

        The lit shape is blurred and screened back over the key, which is what
        makes a backlit plastic button read as *lit* rather than as a brighter
        colour.
        """
        glow = shape.filter(ImageFilter.GaussianBlur(radius=4 * self.scale))
        glow = Image.blend(Image.new("RGBA", image.size, (0, 0, 0, 0)),
                           glow, strength)
        image.alpha_composite(glow)

    def _rect(self, image: Image.Image, place: Place, lit: bool,
              pad: int, hovered: bool = False) -> None:
        s = self.scale
        box = [pad, pad, pad + place.w * s, pad + place.h * s]
        radius = max(2, int(place.h * s * 0.15))
        if place.shape == "round":
            radius = min(place.w, place.h) * s // 2
        accent = place.accent

        if lit:
            fill = lerp(accent, (255, 255, 255), 0.15)
            edge = (255, 255, 255)
            text = (20, 20, 20) if sum(accent) > 320 else (255, 255, 255)
            glow_layer = Image.new("RGBA", image.size, (0, 0, 0, 0))
            ImageDraw.Draw(glow_layer).rounded_rectangle(
                box, radius=radius, fill=(*accent, 210))
            self._bloom(image, glow_layer, accent, 0.85)
        else:
            fill = lerp(BTN, (64, 69, 79), 0.38 if hovered else 0.0)
            edge = lerp(accent, (156, 164, 180) if hovered else BTN_EDGE,
                        0.42 if hovered else 0.55)
            text = lerp(accent, (235, 238, 245) if hovered else BTN_TEXT,
                        0.62 if hovered else 0.45)

        draw = ImageDraw.Draw(image)
        # A small cast shadow and top highlight give the keys a physical
        # resting state without resorting to a loud skeuomorphic texture.
        shadow = [box[0] + s, box[1] + 2 * s,
                  box[2] + s, box[3] + 2 * s]
        draw.rounded_rectangle(shadow, radius=radius, fill=(8, 9, 12, 190))
        draw.rounded_rectangle(box, radius=radius, fill=fill)
        # The double border cdj3k-emu draws on every key: a light outer and a
        # dark inner line is what gives a flat fill an edge.
        draw.rounded_rectangle(box, radius=radius, outline=edge,
                               width=max(1, s // 2))
        draw.rounded_rectangle([box[0] + s, box[1] + s, box[2] - s, box[3] - s],
                               radius=max(1, radius - s), outline=(18, 18, 22),
                               width=max(1, s // 2))
        draw.line([(box[0] + radius, box[1] + s),
                   (box[2] - radius, box[1] + s)],
                  fill=lerp(fill, (255, 255, 255), 0.22),
                  width=max(1, s // 2))
        # The illuminated rail is visible even at rest and blooms with the
        # rest of the control after a click.
        rail = lerp(accent, BTN, 0.56 if not hovered else 0.36)
        if place.shape != "round":
            draw.rounded_rectangle([box[0] + 5 * s, box[1] + 3 * s,
                                    box[2] - 5 * s, box[1] + 4 * s],
                                   radius=max(1, s), fill=rail)
        font = self.font(place.font)
        draw.text(((box[0] + box[2]) / 2, (box[1] + box[3]) / 2), place.label,
                  font=font, fill=text, anchor="mm")

    def _knob(self, image: Image.Image, place: Place, lit: bool,
              pad: int, hovered: bool = False) -> None:
        s = self.scale
        r = place.w * s / 2
        cx = cy = pad + r
        draw = ImageDraw.Draw(image)
        if lit:
            glow_layer = Image.new("RGBA", image.size, (0, 0, 0, 0))
            ImageDraw.Draw(glow_layer).ellipse(
                [cx - r, cy - r, cx + r, cy + r], fill=(200, 200, 220, 190))
            self._bloom(image, glow_layer, WHITE, 0.8)
        for step in range(int(r), 0, -1):
            t = 1 - step / r
            draw.ellipse([cx - step, cy - step, cx + step, cy + step],
                         fill=lerp((70, 70, 80), (34, 34, 40), t))
        draw.ellipse([cx - r, cy - r, cx + r, cy + r],
                     outline=(174, 181, 196) if hovered else (120, 120, 132),
                     width=max(1, s))
        # The detent ticks, so a turn is visible rather than inferred.
        for index in range(24):
            angle = index * math.pi / 12
            inner, outer = r * 0.78, r * 0.94
            draw.line([cx + inner * math.cos(angle), cy + inner * math.sin(angle),
                       cx + outer * math.cos(angle), cy + outer * math.sin(angle)],
                      fill=(96, 96, 108), width=max(1, s // 2))
        draw.ellipse([cx - r * 0.55, cy - r * 0.55, cx + r * 0.55, cy + r * 0.55],
                     fill=(44, 44, 52), outline=(130, 130, 145),
                     width=max(1, s // 2))
        draw.text((cx, cy), "PUSH", font=self.font(7),
                  fill=(210, 210, 225) if lit else (150, 150, 165), anchor="mm")

    def _wheel(self, image: Image.Image, place: Place, lit: bool,
               pad: int, hovered: bool = False) -> None:
        s = self.scale
        r = place.w * s / 2
        cx = cy = pad + r
        draw = ImageDraw.Draw(image)
        for step in range(int(r), 0, -1):
            t = 1 - step / r
            draw.ellipse([cx - step, cy - step, cx + step, cy + step],
                         fill=lerp((58, 58, 66), (30, 30, 36), t))
        draw.ellipse([cx - r, cy - r, cx + r, cy + r],
                     outline=((174, 181, 196) if hovered else
                              ((110, 110, 124) if lit else (74, 74, 84))),
                     width=max(1, s))
        for index in range(48):
            angle = index * math.pi / 24
            inner, outer = r * 0.88, r * 0.97
            draw.line([cx + inner * math.cos(angle),
                       cy + inner * math.sin(angle),
                       cx + outer * math.cos(angle),
                       cy + outer * math.sin(angle)],
                      fill=(88, 92, 104), width=max(1, s // 2))
        # The jog LCD in the middle is a real, separate display on this deck
        # (memory cdj-jog-display-keep-separate); it is drawn as a recess and
        # left empty rather than filled with invented content.
        inner = r * 0.44
        draw.ellipse([cx - inner, cy - inner, cx + inner, cy + inner],
                     fill=(14, 14, 18), outline=(70, 70, 80),
                     width=max(1, s // 2))
        draw.text((cx, cy + inner + 9 * s), "JOG TOUCH", font=self.font(7),
                  fill=(150, 150, 165), anchor="mm")


def placed_ids() -> list[str]:
    """The inputs this deck has a position for."""
    return [name for name, place in PLACEMENTS.items() if place.w]


def unplaced(board_ids: list[str]) -> list[str]:
    """The inputs that must appear in the rack instead.

    Kept as a function rather than a second table so the two can never drift:
    the rack is defined as *everything the deck does not draw*, so an input
    added to `panel_control` shows up somewhere in the window without anyone
    having to remember to add it.  That is the failure this project has had
    twice -- 46 inputs and 38 controls, silently.
    """
    placed = set(placed_ids())
    # The SELECT encoder shares the knob with the push it is built into.
    placed.add("field%d" % ENCODER_FIELD)
    return [name for name in board_ids if name not in placed]


ENCODER_FIELD = 7               # panel_control.ANALOG_CONTROLS[7], measured


class Faceplate(tk.Canvas):
    """The deck as a Tk canvas: chassis underneath, keys and the LCD on top.

    Everything static is one image, built once.  Each key is a small image of
    its own so that lighting it is a swap of that key rather than a repaint of
    the deck, and the LCD is a single image item updated in place -- which is
    the whole reason the picture can keep up now (see `set_frame`).
    """

    def __init__(self, parent: tk.Misc, scale: int,
                 resolve: Callable[[str], object | None],
                 click: Callable[[object], None],
                 rotate: Callable[[int, int], None],
                 long_press: Callable[[object], None] | None = None,
                 hold: Callable[[object], None] | None = None,
                 contact: Callable[[object, bool], bool] | None = None,
                 key_contact: Callable[[object, bool], bool] | None = None) -> None:
        self.scale = scale
        self.renderer = Renderer(2)
        self.assets: dict[object, Image.Image] = {}
        self.resolve = resolve
        self.on_click = click
        self.on_rotate = rotate
        self.on_long_press = long_press or click
        self.on_hold = hold or click
        self.on_contact = contact
        self.on_key_contact = key_contact or contact
        self.active_key: tuple[str, str] | None = None
        self.key_release_timer = None
        self.last_frame = Image.new("RGB", (LCD_W, LCD_H))
        self.latched: set[str] = set()
        super().__init__(parent, width=PANEL_W * scale,
                         height=(PANEL_H + RACK_H) * scale,
                         highlightthickness=0, borderwidth=0,
                         background="#%02x%02x%02x" % CHASSIS,
                         takefocus=True)

        self.chassis_photo = self._photo("chassis", self.renderer.chassis)
        self.create_image(0, 0, image=self.chassis_photo, anchor="nw")

        self.key_photo: dict[str, tuple[ImageTk.PhotoImage,
                                        ImageTk.PhotoImage,
                                        ImageTk.PhotoImage]] = {}
        self.key_item: dict[str, int] = {}
        self.lit_until: dict[str, str] = {}
        for name in placed_ids():
            self._build_key(name)

        self.focused_name = placed_ids()[0]
        self.hovered_name: str | None = None
        self.bind("<FocusIn>", lambda _event: self._show_resting(
            self.focused_name))
        self.bind("<FocusOut>", self._focus_out)
        self.bind("<Left>", lambda _event: self._move_focus(-1, 0))
        self.bind("<Right>", lambda _event: self._move_focus(1, 0))
        self.bind("<Up>", lambda _event: self._move_focus(0, -1))
        self.bind("<Down>", lambda _event: self._move_focus(0, 1))
        for key in ("Return", "space"):
            self.bind(f"<KeyPress-{key}>", self._key_down)
            self.bind(f"<KeyRelease-{key}>", self._key_up)

        # The LCD.  One Tk image for the life of the window; `set_frame` pastes
        # into it.  Building a fresh PhotoImage per frame costs 8.0 ms against
        # 5.2 ms for a paste, and -- worse -- churns a 1.8 MB Tk image 30 times
        # a second, which is what the old window did.
        blank = Image.new("RGB", (round(LCD_W * scale), round(LCD_H * scale)), (0, 0, 0))
        self.lcd_photo = ImageTk.PhotoImage(blank)
        self.create_image(LCD_X * scale, LCD_Y * scale, image=self.lcd_photo,
                          anchor="nw")

        self._drag_angle: float | None = None
        self._drag_started = False
        self.active_pointer: str | None = None
        self.bind("<Button-1>", self._pointer_down)
        self.bind("<B1-Motion>", self._pointer_drag)
        self.bind("<ButtonRelease-1>", self._pointer_up)
        self.bind("<Button-3>", self._pointer_hold)
        self.bind("<Button-2>", self._pointer_hold)

    def _photo(self, key, build) -> ImageTk.PhotoImage:
        if key not in self.assets:
            self.assets[key] = build()
        source = self.assets[key]
        size = (max(1, round(source.width * self.scale / 2)),
                max(1, round(source.height * self.scale / 2)))
        return ImageTk.PhotoImage(source.resize(size, Image.Resampling.LANCZOS))

    def set_scale(self, scale: float) -> None:
        if abs(scale - self.scale) < 0.01:
            return
        for timer in self.lit_until.values():
            self.after_cancel(timer)
        self.lit_until.clear()
        self.scale = scale
        self.delete("all")
        self.configure(width=round(PANEL_W * scale), height=round(PANEL_H * scale))
        self.chassis_photo = self._photo("chassis", self.renderer.chassis)
        self.create_image(0, 0, image=self.chassis_photo, anchor="nw")
        for name in placed_ids():
            self._build_key(name)
        self.lcd_photo = ImageTk.PhotoImage(self.last_frame.resize(
            (round(LCD_W * scale), round(LCD_H * scale)), Image.Resampling.NEAREST))
        self.create_image(LCD_X * scale, LCD_Y * scale, image=self.lcd_photo, anchor="nw")
        for name in self.latched:
            self._show_resting(name)

    def set_latched(self, name: str, down: bool) -> None:
        if down:
            self.latched.add(name)
        else:
            self.latched.discard(name)
        self._show_resting(name)

    # ------------------------------------------------------------- keys --
    def _build_key(self, name: str) -> None:
        place = PLACEMENTS[name]
        scale, pad = self.scale, 10 * self.scale
        unlit = self._photo((name, 0), lambda: self.renderer.key(place, lit=False))
        hover = self._photo((name, 1), lambda: self.renderer.key(place, lit=False, hovered=True))
        lit = self._photo((name, 2), lambda: self.renderer.key(place, lit=True))
        self.key_photo[name] = (unlit, hover, lit)
        item = self.create_image(place.x * scale - pad, place.y * scale - pad,
                                 image=unlit, anchor="nw")
        self.key_item[name] = item
        self.tag_bind(item, "<Enter>",
                      lambda _event, key=name: self._hover(key, True))
        self.tag_bind(item, "<Leave>",
                      lambda _event, key=name: self._hover(key, False))
        if place.shape == "knob":
            # Tk refuses <MouseWheel> on a canvas *item* -- only key, button,
            # motion, enter/leave and virtual events are legal there -- so the
            # wheel is taken on the widget and filtered by position below.
            self.bind("<MouseWheel>", self._knob_wheel)
            self.bind("<Button-4>", lambda e: self._knob_wheel(e, 1))
            self.bind("<Button-5>", lambda e: self._knob_wheel(e, -1))

    def hit_control(self, x: float, y: float) -> str | None:
        """Hit the physical key, never the transparent padding used for glow."""
        x, y = x / self.scale, y / self.scale
        for name, p in PLACEMENTS.items():
            if p.x <= x <= p.x + p.w and p.y <= y <= p.y + p.h:
                if p.shape in ("wheel", "knob", "round"):
                    if ((x-p.x-p.w/2)/(p.w/2))**2 + ((y-p.y-p.h/2)/(p.h/2))**2 > 1:
                        continue
                return name
        return None

    def _pointer_down(self, event) -> None:
        self.cancel_pointer()
        name = self.hit_control(event.x, event.y)
        if name is None:
            return
        if event.state & 4:
            self._modified_press(name, self.on_hold)
        elif event.state & 1:
            self._modified_press(name, self.on_long_press)
        elif PLACEMENTS[name].shape == "knob":
            self.active_pointer = name
            self._knob_down(event)
        elif self.on_contact is not None and not name.endswith("-hold"):
            self.focus_set()
            self.focused_name = name
            control = self.resolve(name)
            if control is not None and self.on_contact(control, True):
                self.active_pointer = name
        else:
            self._pressed(name)

    def _pointer_hold(self, event) -> None:
        name = self.hit_control(event.x, event.y)
        if name is not None:
            self._modified_press(name, self.on_hold)

    def _pointer_drag(self, event) -> None:
        if (self.active_pointer is not None and
                PLACEMENTS[self.active_pointer].shape == "knob"):
            self._knob_drag(event, self.active_pointer)

    def _pointer_up(self, event) -> None:
        if (self.active_pointer is not None and
                PLACEMENTS[self.active_pointer].shape == "knob"):
            self._knob_up(self.active_pointer)
            self.active_pointer = None
        else:
            self.cancel_pointer()

    def cancel_pointer(self) -> None:
        """Release the original key even outside its hit box or after focus loss."""
        name = self.active_pointer
        self.active_pointer = None
        self._drag_angle = None
        self._drag_started = False
        if (name is not None and PLACEMENTS[name].shape != "knob"
                and self.on_contact is not None):
            control = self.resolve(name)
            if control is not None:
                self.on_contact(control, False)

    def _focus_out(self, _event) -> None:
        self.cancel_pointer()
        self.cancel_key()
        self._show_resting(self.focused_name, focused=False)

    def _key_down(self, event) -> str:
        if self.active_key is not None:
            if self.active_key[0] == event.keysym and self.key_release_timer is not None:
                self.after_cancel(self.key_release_timer)
                self.key_release_timer = None
            return "break"  # OS key repeat must not enqueue extra pulses.
        name = self.focused_name
        self.active_key = (event.keysym, name)
        control = self.resolve(name)
        if self.on_key_contact is not None and not name.endswith("-hold"):
            if control is not None:
                self.on_key_contact(control, True)
        else:
            self._pressed(name)
        return "break"

    def _key_up(self, event) -> str:
        if (self.active_key is not None and self.active_key[0] == event.keysym
                and self.key_release_timer is None):
            # Some Tk platforms report auto-repeat as release/press pairs in
            # one event batch. A following press cancels this pending release.
            self.key_release_timer = self.after_idle(self.cancel_key)
        return "break"

    def cancel_key(self) -> None:
        if self.key_release_timer is not None:
            self.after_cancel(self.key_release_timer)
            self.key_release_timer = None
        active, self.active_key = self.active_key, None
        if active is not None and self.on_key_contact is not None:
            name = active[1]
            control = self.resolve(name)
            if control is not None and not name.endswith("-hold"):
                self.on_key_contact(control, False)

    def _modified_press(self, name, callback):
        control = self.resolve(name)
        if control is not None:
            callback(control)
        return "break"

    def _hover(self, name: str, entered: bool) -> None:
        """Give canvas controls the affordance normal Tk buttons get free."""
        self.configure(cursor="hand2" if entered else "")
        self.hovered_name = name if entered else None
        self._show_resting(name)

    def _show_resting(self, name: str, focused: bool = True) -> None:
        """Show hover/focus affordance unless the control is flashing."""
        if name in self.lit_until:
            return
        active = (self.hovered_name == name or
                  (focused and self.focus_get() is self and
                   self.focused_name == name))
        self.itemconfigure(self.key_item[name],
                           image=self.key_photo[name][2 if name in self.latched else (1 if active else 0)])

    def _move_focus(self, dx: int, dy: int) -> str:
        """Move keyboard focus to the closest control in that direction."""
        old = self.focused_name
        origin = PLACEMENTS[old]
        ox, oy = origin.x + origin.w / 2, origin.y + origin.h / 2
        candidates: list[tuple[float, str]] = []
        for name in placed_ids():
            if name == old:
                continue
            place = PLACEMENTS[name]
            x, y = place.x + place.w / 2, place.y + place.h / 2
            forward = (x - ox) * dx + (y - oy) * dy
            if forward <= 0:
                continue
            cross = abs((x - ox) * dy - (y - oy) * dx)
            # Prefer the intended axis strongly, then the nearest control.
            candidates.append((forward + cross * 3.0, name))
        if candidates:
            self.focused_name = min(candidates)[1]
            self._show_resting(old, focused=False)
            self._show_resting(self.focused_name)
        return "break"

    def _pressed(self, name: str) -> None:
        old = self.focused_name
        self.focus_set()
        self.focused_name = name
        if old != name:
            self._show_resting(old, focused=False)
        control = self.resolve(name)
        if control is not None:
            self.on_click(control)
        self.flash(name)

    def flash(self, name: str, milliseconds: int = 190) -> None:
        """Light a key because *this window* sent it, and let it fade.

        Not because the deck lit it: nothing here has ever measured the panel's
        LED lines, so the window shows what it did, not what the firmware
        thinks.  Conflating the two would make the picture a claim about the
        hardware.
        """
        item = self.key_item.get(name)
        if item is None:
            return
        self.itemconfigure(item, image=self.key_photo[name][2])
        pending = self.lit_until.get(name)
        if pending:
            self.after_cancel(pending)
        self.lit_until[name] = self.after(
            milliseconds,
            lambda: self._unflash(name))

    def _unflash(self, name: str) -> None:
        self.lit_until.pop(name, None)
        self._show_resting(name)

    # ------------------------------------------------------------ knob --
    def _knob_down(self, event: tk.Event) -> None:
        self.focus_set()
        self._drag_angle = self._knob_angle(event)
        self._drag_started = False

    def _knob_up(self, name: str) -> None:
        if self._drag_angle is not None and not self._drag_started:
            self._pressed(name)
        self._drag_angle = None

    def _knob_angle(self, event: tk.Event) -> float:
        place = PLACEMENTS[ENCODER_FIELD_PLACE]
        cx = (place.x + place.w / 2) * self.scale
        cy = (place.y + place.h / 2) * self.scale
        return math.atan2(self.canvasy(event.y) - cy, self.canvasx(event.x) - cx)

    def _knob_drag(self, event: tk.Event, name: str) -> None:
        angle = self._knob_angle(event)
        if self._drag_angle is None:
            self._drag_angle = angle
            return
        delta = angle - self._drag_angle
        while delta > math.pi:
            delta -= 2 * math.pi
        while delta < -math.pi:
            delta += 2 * math.pi
        detents = int(delta / (math.pi / 12))       # 24 detents per turn
        if detents:
            self._drag_started = True
            self._drag_angle = angle
            self.on_rotate(ENCODER_FIELD, detents)
            self.flash(name, 90)

    def _knob_wheel(self, event: tk.Event, direction: int | None = None) -> None:
        """Wheel over the knob turns it; wheel anywhere else is not for us.

        The filter matters: the wheel reaches the whole canvas, and a scroll
        aimed at the window would otherwise walk the browse list.
        """
        place = PLACEMENTS[ENCODER_FIELD_PLACE]
        x, y = self.canvasx(event.x), self.canvasy(event.y)
        cx = (place.x + place.w / 2) * self.scale
        cy = (place.y + place.h / 2) * self.scale
        if math.hypot(x - cx, y - cy) > place.w / 2 * self.scale:
            return
        if direction is None and not event.delta:
            return
        self.on_rotate(ENCODER_FIELD, direction or (1 if event.delta > 0 else -1))
        self.flash(ENCODER_FIELD_PLACE, 90)

    # ------------------------------------------------------------- LCD --
    def set_frame(self, frame: Image.Image) -> None:
        """Put a 480x234 panel frame on the deck, scaled by whole pixels."""
        self.last_frame = frame.copy()
        if self.scale != 1:
            frame = frame.resize((round(frame.width * self.scale),
                                  round(frame.height * self.scale)),
                                 Image.Resampling.NEAREST)
        self.lcd_photo.paste(frame)
