#!/usr/bin/env python3
"""Generate the project's original, redistributable fallback resource pack.

Uses only the Python standard library. The PNG textures are drawn from simple
geometric primitives; the short sound cues are synthesized and encoded by an
installed FFmpeg with libmp3lame support.
"""

from __future__ import annotations

import math
import os
import shutil
import struct
import subprocess
import tempfile
import wave
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets" / "respack-open"


class Canvas:
    def __init__(self, width: int, height: int):
        self.width = width
        self.height = height
        self.pixels = bytearray(width * height * 4)

    def over(self, x: int, y: int, color: tuple[int, int, int], alpha: float) -> None:
        if not (0 <= x < self.width and 0 <= y < self.height) or alpha <= 0:
            return
        alpha = min(1.0, alpha)
        i = (y * self.width + x) * 4
        da = self.pixels[i + 3] / 255.0
        oa = alpha + da * (1.0 - alpha)
        if oa <= 0:
            return
        for c in range(3):
            self.pixels[i + c] = round((color[c] * alpha + self.pixels[i + c] * da * (1 - alpha)) / oa)
        self.pixels[i + 3] = round(oa * 255)

    def png(self, path: Path) -> None:
        def chunk(tag: bytes, data: bytes) -> bytes:
            return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

        raw = b"".join(b"\0" + self.pixels[y * self.width * 4 : (y + 1) * self.width * 4] for y in range(self.height))
        data = b"\x89PNG\r\n\x1a\n"
        data += chunk(b"IHDR", struct.pack(">2I5B", self.width, self.height, 8, 6, 0, 0, 0))
        data += chunk(b"IDAT", zlib.compress(raw, 9))
        data += chunk(b"IEND", b"")
        path.write_bytes(data)


def clamp(x: float) -> float:
    return max(0.0, min(1.0, x))


def smooth(a: float, b: float, x: float) -> float:
    t = clamp((x - a) / (b - a))
    return t * t * (3 - 2 * t)


def sd_round_rect(x: float, y: float, x0: float, y0: float, x1: float, y1: float, radius: float) -> float:
    cx, cy = (x0 + x1) * 0.5, (y0 + y1) * 0.5
    qx = abs(x - cx) - ((x1 - x0) * 0.5 - radius)
    qy = abs(y - cy) - ((y1 - y0) * 0.5 - radius)
    return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - radius


def paint_round_rect(
    image: Canvas,
    box: tuple[float, float, float, float],
    radius: float,
    color: tuple[int, int, int],
    border: int = 3,
    glow: int = 20,
    glow_color: tuple[int, int, int] | None = None,
) -> None:
    x0, y0, x1, y1 = box
    glow_color = glow_color or color
    left, top = max(0, int(x0 - glow)), max(0, int(y0 - glow))
    right, bottom = min(image.width, int(x1 + glow + 1)), min(image.height, int(y1 + glow + 1))
    for y in range(top, bottom):
        for x in range(left, right):
            d = sd_round_rect(x + 0.5, y + 0.5, x0, y0, x1, y1, radius)
            if d > 0:
                image.over(x, y, glow_color, 0.22 * math.exp(-d / max(glow * 0.27, 1)))
                continue
            coverage = clamp(0.5 - d)
            if d > -border:
                image.over(x, y, (245, 252, 255), coverage)
                continue
            t = clamp((y - y0) / max(y1 - y0, 1))
            shade = 0.94 - 0.30 * t
            fill = tuple(round(c * shade) for c in color)
            image.over(x, y, fill, coverage)
            # A slim specular strip gives the sprite a beveled glass surface.
            if y < y0 + (y1 - y0) * 0.27 and d < -border - 1:
                image.over(x, y, (255, 255, 255), 0.24 * coverage)


def segment_distance(px: float, py: float, ax: float, ay: float, bx: float, by: float) -> float:
    dx, dy = bx - ax, by - ay
    denom = dx * dx + dy * dy
    t = 0.0 if denom == 0 else clamp(((px - ax) * dx + (py - ay) * dy) / denom)
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def paint_line(image: Canvas, a: tuple[float, float], b: tuple[float, float], width: float, color: tuple[int, int, int], alpha: float = 1.0) -> None:
    reach = width * 0.5 + 2
    for y in range(max(0, int(min(a[1], b[1]) - reach)), min(image.height, int(max(a[1], b[1]) + reach + 1))):
        for x in range(max(0, int(min(a[0], b[0]) - reach)), min(image.width, int(max(a[0], b[0]) + reach + 1))):
            d = segment_distance(x + 0.5, y + 0.5, *a, *b)
            image.over(x, y, color, alpha * clamp(width * 0.5 + 0.5 - d))


def note_sprite(path: Path, color: tuple[int, int, int], kind: str, multi: bool) -> None:
    width = 288 if multi else 256
    sprite_kind = "tap" if kind == "click" else kind
    height = {"tap": 48, "drag": 36, "flick": 64}[sprite_kind]
    image = Canvas(width, height)
    inset = 14 if kind == "flick" else 10
    box = (inset, 7, width - inset, height - 7)
    paint_round_rect(image, box, (height - 14) * (0.48 if kind != "drag" else 0.34), color, 3, 18)
    if kind == "flick":
        # Directional chevron, drawn as a generic geometric cue.
        x = width * 0.70
        paint_line(image, (x - 11, height * 0.31), (x + 1, height * 0.5), 3, (255, 255, 255), 0.96)
        paint_line(image, (x + 1, height * 0.5), (x - 11, height * 0.69), 3, (255, 255, 255), 0.96)
    if multi:
        paint_line(image, (width * 0.29, height * 0.50), (width * 0.43, height * 0.50), 2, (255, 255, 255), 0.9)
        paint_line(image, (width * 0.57, height * 0.50), (width * 0.71, height * 0.50), 2, (255, 255, 255), 0.9)
    image.png(path)


def hold_sprite(path: Path, color: tuple[int, int, int], multi: bool) -> None:
    width, height = (160 if multi else 128), 512
    image = Canvas(width, height)
    x0, x1 = (26 if multi else 18), (width - (26 if multi else 18))
    # One continuous body with inset end caps; the atlas keeps 64px at both ends.
    paint_round_rect(image, (x0, 20, x1, height - 20), 18, color, 3, 14)
    for y in (64, height - 64):
        paint_line(image, (x0 + 6, y), (x1 - 6, y), 2, (255, 255, 255), 0.55)
    if multi:
        paint_line(image, (width * 0.42, 80), (width * 0.42, height - 80), 2, (255, 255, 255), 0.38)
        paint_line(image, (width * 0.58, 80), (width * 0.58, height - 80), 2, (255, 255, 255), 0.38)
    image.png(path)


def effect_cell(index: int, size: int = 64) -> Canvas:
    image = Canvas(size, size)
    frame = index / 55.0
    cx, cy = (size - 1) * 0.5, (size - 1) * 0.5
    radius = 4.0 + 25.0 * frame
    fade = (1.0 - frame) ** 0.75
    for y in range(size):
        for x in range(size):
            dx, dy = x + 0.5 - cx, y + 0.5 - cy
            r = math.hypot(dx, dy)
            ring = math.exp(-abs(r - radius) / 2.6) * fade
            image.over(x, y, (255, 255, 255), 0.80 * ring)
            # Eight short rays fade out as the ring expands.
            angle = math.atan2(dy, dx)
            spoke = abs(math.sin(angle * 4.0))
            if radius - 9 <= r <= radius + 2:
                image.over(x, y, (255, 255, 255), 0.48 * fade * (1 - smooth(0.55, 0.98, spoke)))
    return image


def hit_fx(path: Path) -> None:
    cols, rows, cell = 8, 7, 64
    image = Canvas(cols * cell, rows * cell)
    for i in range(cols * rows):
        frame = effect_cell(i, cell)
        ox, oy = (i % cols) * cell, (i // cols) * cell
        for y in range(cell):
            src = y * cell * 4
            dst = ((oy + y) * image.width + ox) * 4
            image.pixels[dst : dst + cell * 4] = frame.pixels[src : src + cell * 4]
    image.png(path)


def synthesized_wav(path: Path, start_hz: float, end_hz: float, duration: float) -> None:
    rate = 44100
    count = int(rate * duration)
    phase = 0.0
    samples = bytearray()
    for i in range(count):
        t = i / rate
        hz = start_hz + (end_hz - start_hz) * t / duration
        phase += 2 * math.pi * hz / rate
        envelope = min(1.0, t / 0.003) * math.exp(-t * (18 if duration < 0.1 else 8))
        sample = round(0.18 * envelope * math.sin(phase) * 32767)
        samples += struct.pack("<h", sample)
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(rate)
        output.writeframes(samples)


def audio_clips(ffmpeg: str) -> None:
    clips = {
        "click": (1080, 820, 0.040),
        "drag": (660, 460, 0.085),
        "flick": (1320, 880, 0.055),
        "ending": (720, 520, 0.22),
    }
    with tempfile.TemporaryDirectory(prefix="open-respack-") as directory:
        directory = Path(directory)
        for name, (start, end, duration) in clips.items():
            source, target = directory / f"{name}.wav", OUT / f"{name}.mp3"
            synthesized_wav(source, start, end, duration)
            subprocess.run(
                [ffmpeg, "-y", "-hide_banner", "-loglevel", "error", "-i", str(source), "-codec:a", "libmp3lame", "-q:a", "9", "-ar", "48000", "-ac", "2", str(target)],
                check=True,
            )


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    colors = {
        "click": (74, 190, 255),
        "drag": (255, 218, 92),
        "flick": (255, 211, 87),
    }
    for kind, color in colors.items():
        note_sprite(OUT / f"{kind}.png", color, kind, False)
        note_sprite(OUT / f"{kind}_mh.png", tuple(min(255, c + 18) for c in color), kind, True)
    hold_color = (69, 176, 246)
    hold_sprite(OUT / "hold.png", hold_color, False)
    hold_sprite(OUT / "hold_mh.png", tuple(min(255, c + 18) for c in hold_color), True)
    hit_fx(OUT / "hit_fx.png")
    ffmpeg = os.environ.get("PHIRA_FFMPEG") or shutil.which("ffmpeg")
    if not ffmpeg:
        raise SystemExit("未找到 FFmpeg；设置 PHIRA_FFMPEG 或将 ffmpeg 加入 PATH 后重试。")
    audio_clips(ffmpeg)
    print(f"已生成原创默认资源包：{OUT}")


if __name__ == "__main__":
    main()
