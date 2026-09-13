#!/usr/bin/env python3
"""Live lava-lamp simulation for a PIM545 (Pico Scroll Pack) 7 by 17 matrix.

Python 3.10+; no required packages. Optional: pyserial to stream frames to
an ESP32 running the firmware in firmware/pim545_lava_lamp.

The physics match the Green Building lava lamp (17x9 RGB windows), scaled
to 7 columns by 17 rows. The PIM545 LEDs are white, so hardware frames are
luminance; --preview still shows the original colour field.
"""
from __future__ import annotations

import argparse
import colorsys
import math
import random
import struct
import sys
import time
import zlib
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path

WIDTH = 7
HEIGHT = 17
CHANNELS = 3
FRAME_BYTES = WIDTH * HEIGHT * CHANNELS
LUMA_BYTES = WIDTH * HEIGHT
BACKGROUND = (5, 2, 15)
Y_ASPECT = 1.0  # Scroll Pack LEDs are roughly square
GAUSS_FALLOFF = 2.5
GLOW_GAMMA = 1.35
PNG_SCALE = 24
PKT_MAGIC = b"PS"

START_BLOBS = 6
MIN_BLOBS = 5
MAX_BLOBS = 7
MIN_RADIUS = 1.6
MAX_RADIUS = 4.2

X_MIN, X_MAX = 0.3, WIDTH - 1.3
Y_MIN, Y_MAX = 0.3, HEIGHT - 1.3
MAX_SPEED = 1.8
RESTITUTION = 0.85
NEUTRAL_TEMP = 0.5
HEAT_RATE = 0.22
BUOYANCY = 3.4
DRAG = 0.55
WANDER = 0.9
WANDER_Y = 0.28
REPEL_REACH = 0.55
REPEL_STRENGTH = 1.1
MERGE_FACTOR = 0.32
SPLIT_RADIUS = 3.15
SPLIT_TEMP = 0.68
SPLIT_RATE = 0.35
HUE_SPREAD = 0.62
HUE_DRIFT = 0.033
HUE_SPEED_JITTER = 0.024
SAT_BASE = 0.82
SAT_TEMP = 0.18
VAL_BASE = 0.65
VAL_TEMP = 0.35

DRY_RUN_DEFAULT_FRAMES = 600
PROGRESS_EVERY = 2.0

EPILOG = """
The ESP32 firmware runs this lamp on-device. Use --preview here to watch it
in a truecolor terminal, or --serial to push luminance frames over USB.

  python3 lava_lamp.py --preview --frames 0
  python3 lava_lamp.py --serial /dev/cu.usbserial-0001

--fps is both the send rate and the physics timestep. --seed stays
reproducible even if USB is slower than the target rate.
"""


@dataclass
class Blob:
    x: float
    y: float
    vx: float
    vy: float
    radius: float
    hue: float
    hue_speed: float
    phase: float
    temp: float


def ambient_temp(y: float) -> float:
    """Hot at the bottom of the facade, cool at the roof."""
    span = Y_MAX - Y_MIN
    ny = 0.0 if span <= 0 else (y - Y_MIN) / span
    ny = max(0.0, min(1.0, ny))
    return 0.08 + 0.92 * ny


def _mass(blob: Blob) -> float:
    return blob.radius * blob.radius


def spawn_blobs(rng: random.Random, count: int = START_BLOBS) -> list[Blob]:
    blobs = []
    start_hue = rng.random()
    for i in range(count):
        y = rng.uniform(Y_MIN + 0.8, Y_MAX - 0.8)
        blobs.append(
            Blob(
                x=rng.uniform(X_MIN + 0.8, X_MAX - 0.8),
                y=y,
                vx=rng.uniform(-0.45, 0.45),
                vy=rng.uniform(-0.65, 0.65),
                radius=rng.uniform(2.0, 3.4),
                hue=(start_hue + i / max(count, 1) * HUE_SPREAD) % 1.0,
                hue_speed=HUE_DRIFT + rng.uniform(-HUE_SPEED_JITTER, HUE_SPEED_JITTER),
                phase=rng.uniform(0, math.tau),
                temp=max(0.0, min(1.0, ambient_temp(y) + rng.uniform(-0.15, 0.15))),
            )
        )
    return blobs


def _mix_hue(h1: float, w1: float, h2: float, w2: float) -> float:
    x = w1 * math.cos(h1 * math.tau) + w2 * math.cos(h2 * math.tau)
    y = w1 * math.sin(h1 * math.tau) + w2 * math.sin(h2 * math.tau)
    return (math.atan2(y, x) / math.tau) % 1.0


def _integrate(blob: Blob, t: float, dt: float) -> None:
    blob.temp += (ambient_temp(blob.y) - blob.temp) * min(1.0, HEAT_RATE * dt)
    blob.temp = max(0.0, min(1.0, blob.temp))
    blob.hue = (blob.hue + blob.hue_speed * dt) % 1.0
    blob.vx += math.sin(t * 0.73 + blob.phase) * WANDER * dt
    blob.vy += math.sin(t * 0.41 + blob.phase * 1.3) * WANDER_Y * dt
    blob.vy += (NEUTRAL_TEMP - blob.temp) * BUOYANCY * dt
    blob.vx -= blob.vx * DRAG * dt
    blob.vy -= blob.vy * DRAG * dt


def _repel(blobs: list[Blob], dt: float) -> None:
    for i, a in enumerate(blobs):
        for b in blobs[i + 1 :]:
            dx = b.x - a.x
            dy = b.y - a.y
            dist = math.hypot(dx, dy)
            reach = (a.radius + b.radius) * REPEL_REACH
            if 0 < dist < reach:
                force = (reach - dist) * REPEL_STRENGTH * dt
                ux, uy = dx / dist, dy / dist
                a.vx -= force * ux
                a.vy -= force * uy
                b.vx += force * ux
                b.vy += force * uy


def _bounce(blob: Blob, dt: float) -> None:
    blob.vx = max(-MAX_SPEED, min(MAX_SPEED, blob.vx))
    blob.vy = max(-MAX_SPEED, min(MAX_SPEED, blob.vy))
    blob.x += blob.vx * dt
    blob.y += blob.vy * dt
    if blob.x < X_MIN or blob.x > X_MAX:
        blob.x = max(X_MIN, min(X_MAX, blob.x))
        blob.vx *= -RESTITUTION
    if blob.y < Y_MIN or blob.y > Y_MAX:
        blob.y = max(Y_MIN, min(Y_MAX, blob.y))
        blob.vy *= -RESTITUTION
    blob.radius = max(MIN_RADIUS, min(MAX_RADIUS, blob.radius))


def _try_merges(blobs: list[Blob]) -> list[Blob]:
    blobs = list(blobs)
    while len(blobs) > MIN_BLOBS:
        best: tuple[int, int] | None = None
        best_dist = 0.0
        for i, a in enumerate(blobs):
            for j in range(i + 1, len(blobs)):
                b = blobs[j]
                dist = math.hypot(b.x - a.x, b.y - a.y)
                if dist < MERGE_FACTOR * (a.radius + b.radius):
                    if best is None or dist < best_dist:
                        best, best_dist = (i, j), dist
        if best is None:
            break
        i, j = best
        a, b = blobs[i], blobs[j]
        m1, m2 = _mass(a), _mass(b)
        total = m1 + m2
        merged = Blob(
            x=(a.x * m1 + b.x * m2) / total,
            y=(a.y * m1 + b.y * m2) / total,
            vx=(a.vx * m1 + b.vx * m2) / total,
            vy=(a.vy * m1 + b.vy * m2) / total,
            radius=min(MAX_RADIUS, math.sqrt(total)),
            hue=_mix_hue(a.hue, m1, b.hue, m2),
            hue_speed=(a.hue_speed * m1 + b.hue_speed * m2) / total,
            phase=a.phase,
            temp=(a.temp * m1 + b.temp * m2) / total,
        )
        blobs.pop(j)
        blobs.pop(i)
        blobs.append(merged)
    return blobs


def _try_splits(blobs: list[Blob], rng: random.Random, dt: float) -> list[Blob]:
    out: list[Blob] = []
    remaining = list(blobs)
    while remaining:
        blob = remaining.pop()
        live = len(out) + 1 + len(remaining)
        if (
            live < MAX_BLOBS
            and blob.radius > SPLIT_RADIUS
            and blob.temp > SPLIT_TEMP
            and rng.random() < SPLIT_RATE * dt
        ):
            angle = rng.uniform(0, math.tau)
            offset = blob.radius * 0.35
            radius = max(MIN_RADIUS, blob.radius / math.sqrt(2))
            for sign in (-1, 1):
                child = Blob(
                    x=blob.x + sign * math.cos(angle) * offset,
                    y=blob.y + sign * math.sin(angle) * offset,
                    vx=blob.vx + sign * math.cos(angle) * 0.3,
                    vy=blob.vy + sign * math.sin(angle) * 0.3,
                    radius=radius,
                    hue=(blob.hue + sign * rng.uniform(0.06, 0.14)) % 1.0,
                    hue_speed=blob.hue_speed + sign * rng.uniform(0.002, 0.01),
                    phase=blob.phase + sign * 0.7,
                    temp=blob.temp,
                )
                _bounce(child, 0.0)
                out.append(child)
        else:
            out.append(blob)
        if len(out) + len(remaining) >= MAX_BLOBS:
            out.extend(remaining)
            break
    return out


def step_blobs(blobs: list[Blob], rng: random.Random, t: float, dt: float) -> list[Blob]:
    for blob in blobs:
        _integrate(blob, t, dt)
    _repel(blobs, dt)
    for blob in blobs:
        _bounce(blob, dt)
    blobs = _try_merges(blobs)
    return _try_splits(blobs, rng, dt)


def pixel_index(x: int, y: int) -> int:
    return (y * WIDTH + x) * CHANNELS


def render_frame(blobs: list[Blob]) -> bytes:
    colors = [
        colorsys.hsv_to_rgb(
            blob.hue % 1.0,
            max(0.0, min(1.0, SAT_BASE - SAT_TEMP * blob.temp)),
            max(0.0, min(1.0, VAL_BASE + VAL_TEMP * blob.temp)),
        )
        for blob in blobs
    ]
    pixels = bytearray()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            weights = [
                math.exp(
                    -GAUSS_FALLOFF
                    * ((x - blob.x) ** 2 + ((y - blob.y) * Y_ASPECT) ** 2)
                    / blob.radius**2
                )
                for blob in blobs
            ]
            total = sum(weights)
            glow = min(1.0, total**GLOW_GAMMA)
            for channel, base in enumerate(BACKGROUND):
                color = sum(w * c[channel] for w, c in zip(weights, colors)) / max(total, 1e-12)
                pixels.append(round(base * (1 - glow) + 255 * color * glow))
    return bytes(pixels)


def rgb_to_luma(pixels: bytes) -> bytes:
    out = bytearray(LUMA_BYTES)
    for i in range(LUMA_BYTES):
        r, g, b = pixels[i * 3], pixels[i * 3 + 1], pixels[i * 3 + 2]
        out[i] = round(0.2126 * r + 0.7152 * g + 0.0722 * b)
    return bytes(out)


def luma_to_rgb(luma: bytes) -> bytes:
    pixels = bytearray()
    for v in luma:
        pixels.extend((v, v, v))
    return bytes(pixels)


def pack_frame(luma: bytes) -> bytes:
    header = PKT_MAGIC + bytes((WIDTH, HEIGHT))
    payload = header + luma
    chk = 0
    for byte in payload:
        chk ^= byte
    return payload + bytes((chk,))


def frames(fps: int = 20, seed: int | None = None) -> Iterator[bytes]:
    rng = random.Random(seed)
    blobs = spawn_blobs(rng)
    dt = 1 / fps
    t = 0.0
    while True:
        blobs = step_blobs(blobs, rng, t, dt)
        yield render_frame(blobs)
        t += dt


def ansi_preview(pixels: bytes) -> str:
    reset = "\x1b[0m"
    rows = []
    for y in range(HEIGHT):
        cells = []
        for x in range(WIDTH):
            i = pixel_index(x, y)
            r, g, b = pixels[i], pixels[i + 1], pixels[i + 2]
            cells.append(f"\x1b[48;2;{r};{g};{b}m  ")
        rows.append("".join(cells) + reset)
    return "\n".join(rows)


def write_ppm(path: str | Path, pixels: bytes, width: int = WIDTH, height: int = HEIGHT) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(f"P6\n{width} {height}\n255\n".encode("ascii") + pixels)


def upscale_rgb(pixels: bytes, width: int, height: int, scale: int) -> tuple[int, int, bytes]:
    out_w, out_h = width * scale, height * scale
    out = bytearray(out_w * out_h * CHANNELS)
    for y in range(height):
        for x in range(width):
            src = pixels[pixel_index(x, y) : pixel_index(x, y) + CHANNELS]
            for dy in range(scale):
                start = ((y * scale + dy) * out_w + x * scale) * CHANNELS
                for dx in range(scale):
                    o = start + dx * CHANNELS
                    out[o : o + CHANNELS] = src
    return out_w, out_h, bytes(out)


def write_png(path: str | Path, width: int, height: int, rgb: bytes) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)

    def chunk(tag: bytes, data: bytes) -> bytes:
        crc = zlib.crc32(tag + data) & 0xFFFFFFFF
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)

    raw = bytearray()
    row = width * CHANNELS
    for y in range(height):
        raw.append(0)
        raw.extend(rgb[y * row : (y + 1) * row])
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    path.write_bytes(png)


def horizontal_strip(
    frame_list: list[bytes], scale: int = 12, gap: int = 2
) -> tuple[int, int, bytes]:
    if not frame_list:
        raise ValueError("frame_list must be non-empty")
    scaled = [upscale_rgb(frame, WIDTH, HEIGHT, scale) for frame in frame_list]
    frame_w, frame_h = scaled[0][0], scaled[0][1]
    count = len(scaled)
    out_w = count * frame_w + (count - 1) * gap
    out_h = frame_h
    out = bytearray(BACKGROUND * (out_w * out_h))
    for index, (_, _, rgb) in enumerate(scaled):
        x0 = index * (frame_w + gap)
        for y in range(frame_h):
            src = y * frame_w * CHANNELS
            dst = (y * out_w + x0) * CHANNELS
            out[dst : dst + frame_w * CHANNELS] = rgb[src : src + frame_w * CHANNELS]
    return out_w, out_h, bytes(out)


def frame_limit(frame_count: int | None, dry_run: bool) -> int:
    if frame_count is not None:
        return frame_count
    if dry_run:
        return DRY_RUN_DEFAULT_FRAMES
    return 0


def fps_type(value: str) -> int:
    try:
        fps = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("fps must be an integer") from error
    if not 1 <= fps <= 30:
        raise argparse.ArgumentTypeError("fps must be between 1 and 30")
    return fps


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog=EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--fps",
        type=fps_type,
        default=20,
        metavar="1-30",
        help="Target send rate and physics timestep (default: 20)",
    )
    parser.add_argument("--seed", type=int, help="Reproducible simulation seed")
    parser.add_argument(
        "--frames",
        type=int,
        default=None,
        help="Stop after N frames. Default: 600 with --dry-run / local dumps, "
        "unlimited while streaming or with --frames 0",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Generate and validate frames without a display",
    )
    parser.add_argument(
        "--preview",
        action="store_true",
        help="Draw each frame as an ANSI color grid on stderr",
    )
    parser.add_argument(
        "--mono",
        action="store_true",
        help="Preview / dump luminance only, matching the white LED matrix",
    )
    parser.add_argument(
        "--serial",
        nargs="?",
        const="auto",
        metavar="PORT",
        help="Stream luminance frames to the ESP32 firmware. "
        "Pass a device such as /dev/cu.usbserial-0001, or omit the path to auto-detect",
    )
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud (default: 115200)")
    parser.add_argument(
        "--dump-ppm",
        metavar="DIR",
        help="Write frame-0001.ppm, frame-0002.ppm, ... into DIR",
    )
    parser.add_argument(
        "--dump-png",
        metavar="PATH",
        help=f"Write a {PNG_SCALE}x nearest-neighbor PNG of the last generated frame",
    )
    parser.add_argument(
        "--dump-strip",
        metavar="PATH",
        help="Write a horizontal PNG strip of generated frames (sampled evenly)",
    )
    return parser


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.frames is not None and args.frames < 0:
        parser.error("--frames must be nonnegative")
    if args.baud <= 0:
        parser.error("--baud must be positive")
    if args.dry_run and args.serial:
        parser.error("--dry-run cannot be combined with --serial")
    if args.dump_strip and args.frames == 0:
        parser.error("--dump-strip requires a finite --frames count")
    streaming = bool(args.serial)
    local_only = bool(args.dry_run or args.preview or args.dump_ppm or args.dump_png or args.dump_strip)
    if not streaming and not local_only:
        parser.error("pass --preview, --dry-run, --serial, or a --dump-* option")
    args._streaming = streaming
    args._limit = frame_limit(args.frames, args.dry_run or not streaming)
    return args


def open_serial(port: str, baud: int):
    try:
        import serial  # type: ignore
        from serial.tools import list_ports  # type: ignore
    except ImportError as error:
        raise SystemExit("pip install pyserial  (needed for --serial)") from error

    if port == "auto":
        ports = list(list_ports.comports())
        candidates = [
            p.device
            for p in ports
            if any(
                token in (p.device + " " + (p.description or "")).lower()
                for token in ("usb", "wch", "cp210", "ch340", "uart", "serial", "esp")
            )
        ]
        if not candidates:
            names = ", ".join(p.device for p in ports) or "(none)"
            raise SystemExit(f"No USB serial device found. Ports: {names}")
        if len(candidates) > 1:
            raise SystemExit("Multiple USB ports: " + ", ".join(candidates) + "; select one with --serial PORT")
        port = candidates[0]
        print(f"Using {port}", flush=True)
    try:
        return serial.Serial(port, baud, timeout=0.1, write_timeout=2)
    except (OSError, ValueError) as error:
        raise SystemExit(f"Cannot open serial port {port}: {error}. Close other serial clients and check the cable/port.") from error


def main(argv: list[str] | None = None) -> None:
    args = parse_args(argv)
    ser = None
    sent = 0
    generated = 0
    last_pixels: bytes | None = None
    strip_frames: list[bytes] = []
    limit = args._limit
    streaming = args._streaming
    if args.serial:
        ser = open_serial(args.serial, args.baud)
        print(f"Streaming 7x17 luma frames to {ser.port} at {args.fps} fps", flush=True)
    elif args.dry_run or args.preview or args.dump_ppm or args.dump_png or args.dump_strip:
        print("Generating locally" + (" (preview on stderr)" if args.preview else ""), flush=True)

    started_run = time.monotonic()
    last_report = started_run
    cursor_hidden = False
    try:
        if ser is not None:
            time.sleep(0.4)
            ser.reset_input_buffer()
        for pixels in frames(args.fps, args.seed):
            loop_started = time.monotonic()
            if len(pixels) != FRAME_BYTES:
                raise RuntimeError(f"expected {FRAME_BYTES}-byte frames, got {len(pixels)}")
            generated += 1
            display = luma_to_rgb(rgb_to_luma(pixels)) if args.mono else pixels
            last_pixels = display
            if args.dump_ppm:
                write_ppm(Path(args.dump_ppm) / f"frame-{generated:04d}.ppm", display)
            if args.dump_strip is not None:
                strip_frames.append(display)
            if args.preview:
                if not cursor_hidden:
                    sys.stderr.write("\x1b[?25l")
                    cursor_hidden = True
                elapsed = max(time.monotonic() - started_run, 1e-9)
                sys.stderr.write(
                    "\x1b[H\x1b[2J"
                    + ansi_preview(display)
                    + f"\n{generated} frames  {generated / elapsed:.1f} fps\n"
                )
                sys.stderr.flush()
            if ser is not None:
                packet = pack_frame(rgb_to_luma(pixels))
                if ser.write(packet) != len(packet):
                    raise OSError("Incomplete USB frame write")
                # Drain firmware diagnostics so its serial output cannot back up.
                if ser.in_waiting:
                    message = ser.read(min(ser.in_waiting, 4096)).decode(errors="replace").strip()
                    if message:
                        print(f"ESP32: {message}", file=sys.stderr)
                time.sleep(max(0, 1 / args.fps - (time.monotonic() - loop_started)))
            elif args.preview:
                time.sleep(max(0, 1 / args.fps - (time.monotonic() - loop_started)))
            sent += 1
            now = time.monotonic()
            if not args.preview and now - last_report >= PROGRESS_EVERY:
                elapsed = max(now - started_run, 1e-9)
                verb = "sent" if ser is not None else "validated"
                print(f"{sent} frames {verb} ({sent / elapsed:.1f} fps)", flush=True)
                last_report = now
            if limit and sent >= limit:
                break
    except KeyboardInterrupt:
        print("\nStopped.")
    except OSError as error:
        raise SystemExit(f"Output failed: {error}. Check the USB connection/port or output path, then restart.") from error
    finally:
        if cursor_hidden:
            sys.stderr.write("\x1b[?25h")
            sys.stderr.flush()
        if ser is not None:
            ser.close()
        if args.dump_png and last_pixels is not None:
            out_w, out_h, rgb = upscale_rgb(last_pixels, WIDTH, HEIGHT, PNG_SCALE)
            write_png(args.dump_png, out_w, out_h, rgb)
        if args.dump_strip and strip_frames:
            # Keep about six stills across the run.
            if len(strip_frames) > 6:
                step = max(1, (len(strip_frames) - 1) // 5)
                strip_frames = strip_frames[::step][:6]
            out_w, out_h, rgb = horizontal_strip(strip_frames)
            write_png(args.dump_strip, out_w, out_h, rgb)
    verb = "sent" if streaming else "validated"
    print(f"{sent} frames {verb}.", flush=True)


if __name__ == "__main__":
    main()
