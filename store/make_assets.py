#!/usr/bin/env python3
"""
Regenerate the appstore artwork: app icons and the marketing banner.

    python3 store/make_assets.py

Needs Pillow and the Inter font (falls back to DejaVu Sans). Screenshots are
not generated here; they are captured from the emery emulator into
store/screenshots/ (see store/LISTING.md).
"""

import os
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

NAVY = (11, 31, 58)
NAVY_LIGHT = (24, 52, 92)
WHITE = (255, 255, 255)
ORANGE = (255, 138, 61)
GREY = (176, 190, 210)


def font(weight, size):
    for path in (
        "/usr/share/fonts/opentype/inter/Inter-{}.otf".format(weight),
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
        if weight in ("Bold", "SemiBold") else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    ):
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def draw_icon(size):
    """Front of a train on rails, drawn at 4x and downsampled for clean edges."""
    s = size * 4
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    u = s / 144.0  # design grid is 144 units

    def box(x0, y0, x1, y1):
        return [x0 * u, y0 * u, x1 * u, y1 * u]

    d.rounded_rectangle(box(0, 0, 144, 144), radius=32 * u, fill=NAVY)
    # Rails, converging towards the train.
    d.polygon([(40 * u, 134 * u), (54 * u, 134 * u), (62 * u, 112 * u), (58 * u, 112 * u)], fill=ORANGE)
    d.polygon([(104 * u, 134 * u), (90 * u, 134 * u), (82 * u, 112 * u), (86 * u, 112 * u)], fill=ORANGE)
    # Body.
    d.rounded_rectangle(box(38, 22, 106, 108), radius=18 * u, fill=WHITE)
    # Windscreen.
    d.rounded_rectangle(box(48, 34, 96, 66), radius=8 * u, fill=NAVY)
    # Headlights.
    d.ellipse(box(50, 80, 62, 92), fill=ORANGE)
    d.ellipse(box(82, 80, 94, 92), fill=ORANGE)
    # Destination sign.
    d.rounded_rectangle(box(58, 26, 86, 30), radius=2 * u, fill=NAVY_LIGHT)
    return img.resize((size, size), Image.LANCZOS)


def watch(screenshot_path):
    """A screenshot inside a simple Pebble Time 2-ish bezel."""
    shot = Image.open(screenshot_path).convert("RGB")
    pad = 12
    w, h = shot.size[0] + pad * 2, shot.size[1] + pad * 2
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([0, 0, w - 1, h - 1], radius=22, fill=(18, 18, 20))
    d.rounded_rectangle([3, 3, w - 4, h - 4], radius=19, outline=(60, 60, 66), width=2)
    img.paste(shot, (pad, pad))
    return img


def drop_shadow(canvas, im, xy, blur_offset=6):
    shadow = Image.new("RGBA", im.size, (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle(
        [0, 0, im.size[0] - 1, im.size[1] - 1], radius=22, fill=(0, 0, 0, 110))
    canvas.alpha_composite(shadow, (xy[0] + blur_offset, xy[1] + blur_offset))
    canvas.alpha_composite(im, xy)


def banner():
    w, h = 720, 320
    img = Image.new("RGBA", (w, h), NAVY + (255,))
    d = ImageDraw.Draw(img)
    # Soft diagonal band for depth.
    d.polygon([(420, 0), (720, 0), (720, 320), (300, 320)], fill=NAVY_LIGHT)

    icon = draw_icon(72)
    img.alpha_composite(icon, (36, 44))

    d.text((36, 136), "Entur", font=font("Bold", 44), fill=WHITE)
    d.text((36, 184), "Departures", font=font("Bold", 44), fill=WHITE)
    d.text((38, 244), "Your trains, the right way round.", font=font("Medium", 17), fill=GREY)
    d.text((38, 272), "Live Norwegian public transport", font=font("Regular", 14), fill=GREY)

    back = watch(os.path.join(HERE, "screenshots", "emery-3-big-text.png"))
    front = watch(os.path.join(HERE, "screenshots", "emery-2-board.png"))
    drop_shadow(img, back, (478, 20))
    drop_shadow(img, front, (348, 48))
    return img.convert("RGB")


def main():
    for size in (80, 144):
        icon = draw_icon(size)
        icon.save(os.path.join(ROOT, "icon_{0}x{0}.png".format(size)))
    banner().save(os.path.join(HERE, "banner_720x320.png"))
    print("icons + banner written")


if __name__ == "__main__":
    main()
