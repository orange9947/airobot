#!/usr/bin/env python3
"""Generate deterministic 128x64 monochrome sample expression frames."""

import argparse
import json
from pathlib import Path

from PIL import Image, ImageDraw


WIDTH = 128
HEIGHT = 64
EXPRESSIONS = (
    "NEUTRAL",
    "HAPPY",
    "SAD",
    "THINKING",
    "SURPRISED",
    "SLEEPY",
)
INTERVALS = {
    "NEUTRAL": (180, 140),
    "HAPPY": (120, 100),
    "SAD": (180, 150),
    "THINKING": (160, 130),
    "SURPRISED": (100, 80),
    "SLEEPY": (260, 220),
}


def _line(draw, points, width=2):
    draw.line(points, fill=1, width=width)


def _eyes_open(draw, phase, style, pupil_y=24):
    gaze = (-2, 0, 2)[phase]
    if style == 0:
        draw.rounded_rectangle((29, 16, 49, 32), radius=5, outline=1, width=2)
        draw.rounded_rectangle((79, 16, 99, 32), radius=5, outline=1, width=2)
    else:
        draw.ellipse((28, 15, 50, 33), outline=1, width=2)
        draw.ellipse((78, 15, 100, 33), outline=1, width=2)
    draw.ellipse((36 + gaze, pupil_y - 3, 42 + gaze, pupil_y + 3), fill=1)
    draw.ellipse((86 + gaze, pupil_y - 3, 92 + gaze, pupil_y + 3), fill=1)


def _draw_neutral(draw, phase, style):
    if style == 1 and phase == 1:
        _line(draw, ((29, 25), (49, 25)), 3)
        _line(draw, ((79, 25), (99, 25)), 3)
    else:
        _eyes_open(draw, phase, style)
    mouth_half = (8, 11, 9)[phase]
    _line(draw, ((64 - mouth_half, 44), (64 + mouth_half, 44)), 2)
    if style == 1:
        draw.point((64, 48), fill=1)


def _draw_happy(draw, phase, style):
    lift = (0, 1, 0)[phase]
    if style == 0:
        draw.arc((28, 18 - lift, 50, 32 - lift), 190, 350, fill=1, width=3)
        draw.arc((78, 18 - lift, 100, 32 - lift), 190, 350, fill=1, width=3)
    else:
        _line(draw, ((29, 27), (39, 20 - lift), (49, 27)), 3)
        _line(draw, ((79, 27), (89, 20 - lift), (99, 27)), 3)
    inset = (0, 2, 1)[phase]
    draw.arc((48 + inset, 31, 80 - inset, 52), 10, 170, fill=1, width=3)
    draw.ellipse((24, 35, 29, 38), fill=1)
    draw.ellipse((99, 35, 104, 38), fill=1)


def _draw_sad(draw, phase, style):
    _eyes_open(draw, 2 - phase, style, pupil_y=26)
    _line(draw, ((29, 17), (48, 13 + phase)), 2)
    _line(draw, ((80, 13 + phase), (99, 17)), 2)
    width = (24, 20, 16)[phase]
    draw.arc((64 - width // 2, 40, 64 + width // 2, 54), 190, 350,
             fill=1, width=2)
    tear_y = 32 + phase * 2
    if style == 0:
        _line(draw, ((101, tear_y), (101, tear_y + 5)), 2)
        draw.point((100, tear_y + 6), fill=1)
    else:
        _line(draw, ((26, tear_y), (25, tear_y + 5)), 2)


def _draw_thinking(draw, phase, style):
    gaze = (2, -2, 0)[phase]
    draw.rounded_rectangle((29, 17, 49, 31), radius=4, outline=1, width=2)
    draw.ellipse((37 + gaze, 21, 43 + gaze, 27), fill=1)
    if style == 0:
        _line(draw, ((79, 26), (99, 26)), 3)
        _line(draw, ((80, 18 + phase), (98, 16)), 2)
    else:
        draw.ellipse((80, 18, 98, 31), outline=1, width=2)
        draw.ellipse((86 - gaze, 22, 92 - gaze, 28), fill=1)
    shift = phase * 2 - 2
    for x in (56, 64, 72):
        draw.ellipse((x + shift, 43, x + shift + 3, 46), fill=1)
    draw.arc((21, 35, 35, 49), 240, 70, fill=1, width=2)


def _draw_surprised(draw, phase, style):
    grow = phase + style
    draw.ellipse((29 - grow, 14 - grow, 49 + grow, 34 + grow),
                 outline=1, width=3)
    draw.ellipse((79 - grow, 14 - grow, 99 + grow, 34 + grow),
                 outline=1, width=3)
    draw.ellipse((37, 22, 42, 27), fill=1)
    draw.ellipse((87, 22, 92, 27), fill=1)
    mouth_width = 10 + phase * 2
    mouth_height = 12 + phase * 2
    draw.ellipse((64 - mouth_width // 2, 38,
                  64 + mouth_width // 2, 38 + mouth_height),
                 outline=1, width=2)
    if style == 1:
        _line(draw, ((21, 20), (16, 17)), 2)
        _line(draw, ((107, 20), (112, 17)), 2)


def _draw_sleepy(draw, phase, style):
    y = 24 + (1 if phase == 1 else 0)
    if style == 0:
        draw.arc((28, y - 5, 50, y + 5), 10, 170, fill=1, width=3)
        draw.arc((78, y - 5, 100, y + 5), 10, 170, fill=1, width=3)
    else:
        _line(draw, ((29, y), (49, y + 2)), 3)
        _line(draw, ((79, y + 2), (99, y)), 3)
    mouth_width = (13, 9, 11)[phase]
    _line(draw, ((64 - mouth_width // 2, 43),
                 (64 + mouth_width // 2, 43)), 2)
    if phase == 2:
        draw.ellipse((61, 47, 67, 50), outline=1)
    if style == 1:
        draw.arc((18, 34, 30, 48), 250, 80, fill=1, width=2)


DRAWERS = {
    "NEUTRAL": _draw_neutral,
    "HAPPY": _draw_happy,
    "SAD": _draw_sad,
    "THINKING": _draw_thinking,
    "SURPRISED": _draw_surprised,
    "SLEEPY": _draw_sleepy,
}


def render_frame(expression, phase, style):
    image = Image.new("1", (WIDTH, HEIGHT), 0)
    draw = ImageDraw.Draw(image)
    DRAWERS[expression](draw, phase, style)
    return image


def generate(output):
    output = Path(output)
    frames = output / "frames"
    frames.mkdir(parents=True, exist_ok=True)
    clips = []
    for expression in EXPRESSIONS:
        for style in range(2):
            filenames = []
            for phase in range(3):
                filename = "{}_{}_{}.png".format(
                    expression.lower(), style + 1, phase + 1
                )
                render_frame(expression, phase, style).save(
                    frames / filename, format="PNG", optimize=False
                )
                filenames.append("frames/" + filename)
            clips.append(
                {
                    "expression": expression,
                    "weight": 3 if style == 0 else 1,
                    "frame_interval_ms": INTERVALS[expression][style],
                    "frames": filenames,
                }
            )
    manifest = {"clips": clips}
    (output / "manifest.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=True) + "\n",
        encoding="utf-8",
    )
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Generate deterministic sample OLED expression PNGs"
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("examples/resources/default_faces"),
    )
    args = parser.parse_args(argv)
    manifest = generate(args.output)
    print(
        "generated {} clips and {} frames -> {}".format(
            len(manifest["clips"]),
            sum(len(clip["frames"]) for clip in manifest["clips"]),
            args.output,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
