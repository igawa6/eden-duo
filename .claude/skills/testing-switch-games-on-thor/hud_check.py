"""Flags frames whose HUD is missing in a screen recording (default regions: FF Resonance battle).

Usage: python hud_check.py <video> [out_dir] [--region name,x0,y0,x1,y1 ...]

Regions are in 480x270 space (a 1920x1080 frame divided by 4) and should cover HUD panels whose
colour is "panel blue" (blue clearly above red). Pass --region once per panel to replace the
defaults for another game.

Every decoded frame (no frame-rate conversion) is scaled to 480x270 and, for each HUD panel region,
the share of "panel blue" pixels is measured. A frame whose panels lose most of their blue against
the recording's median is reported; with out_dir, those frames and their neighbours are saved.
"""
import subprocess
import sys
from pathlib import Path

import numpy as np

W, H = 480, 270
# (name, x0, y0, x1, y1) in 480x270 space, from a 1920x1080 battle frame.
REGIONS = [
    ("party panels", 355, 225, 475, 262),
    ("command menu", 350, 25, 470, 95),
]


def panel_blue(frame, region):
    _, x0, y0, x1, y1 = region
    px = frame[y0:y1, x0:x1].astype(np.int16)
    r, g, b = px[..., 0], px[..., 1], px[..., 2]
    return float(((b > r + 12) & (b > 40)).mean())


def main():
    args = sys.argv[1:]
    regions = []
    while "--region" in args:
        i = args.index("--region")
        name, *box = args[i + 1].split(",")
        regions.append((name, *map(int, box)))
        del args[i:i + 2]
    if regions:
        REGIONS[:] = regions
    video = args[0]
    out = Path(args[1]) if len(args) > 1 else None
    cmd = ["ffmpeg", "-v", "error", "-i", video, "-fps_mode", "passthrough",
           "-vf", f"scale={W}:{H}", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    frames, scores = [], []
    size = W * H * 3
    while True:
        buf = proc.stdout.read(size)
        if len(buf) < size:
            break
        frame = np.frombuffer(buf, np.uint8).reshape(H, W, 3)
        frames.append(frame)
        scores.append([panel_blue(frame, r) for r in REGIONS])
    proc.wait()
    scores = np.array(scores)
    if len(scores) == 0:
        print("no frames decoded")
        return 2
    median = np.median(scores, axis=0)
    print(f"{len(scores)} frames; median panel blue: " +
          ", ".join(f"{r[0]} {m:.2f}" for r, m in zip(REGIONS, median)))
    # A dropped HUD loses both panels at once; one panel alone is usually a menu change.
    bad = [i for i, s in enumerate(scores)
           if all(s[k] < 0.35 * median[k] for k in range(len(REGIONS)) if median[k] > 0.05)]
    print(f"frames with the HUD missing: {len(bad)}" + (f" -> {bad[:40]}" if bad else ""))
    # The game hides its HUD for whole stretches (attacks, transitions); a dropout is a missing
    # frame with the HUD shown on both sides of it within 3 frames.
    shown = set(range(len(scores))) - set(bad)
    isolated = [i for i in bad
                if any(j in shown for j in range(i - 3, i)) and any(j in shown for j in range(i + 1, i + 4))]
    print(f"isolated dropouts (HUD shown on both sides): {len(isolated)}" +
          (f" -> {isolated[:40]}" if isolated else ""))
    if out and bad:
        out.mkdir(parents=True, exist_ok=True)
        from PIL import Image
        for i in bad[:12]:
            strip = np.concatenate([frames[j] for j in range(max(0, i - 1), min(len(frames), i + 2))], axis=1)
            Image.fromarray(strip).save(out / f"hud-missing-{i:05d}.png")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
