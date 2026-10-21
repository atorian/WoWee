"""Compare the retained GPU captures; never retouch the rendered pixels."""
import json
import re
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFont

root = Path(__file__).resolve().parents[2]
out = root / "build-dev/scratch/aerial-march"
font = ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf", 23)
report = {}
for scene in ("hyjal_day", "citadel_evening"):
    images = [Image.open(out / f"{scene}-{variant}.png").convert("RGB")
              for variant in ("before", "after")]
    a, b = [np.asarray(im).astype(np.int16) for im in images]
    delta = np.abs(a - b)
    stats = {"mean_absolute_rgb_8bit": float(delta.mean()),
             "max_rgb_8bit": int(delta.max()),
             "changed_pixel_percent": float(100 * np.any(delta, axis=2).mean()),
             "unchanged_sky_max_difference": int(delta[:80].max())}
    for variant in ("before", "after", "reference"):
        log = out / f"{scene}-{variant}.log"
        if not log.exists(): continue
        raw = log.read_bytes()
        text = raw.decode("utf-16" if raw.startswith(b"\xff\xfe") else "utf-8-sig")
        stats[variant + "_gpu"] = [line for line in text.splitlines() if "GPU," in line]
        stats[variant + "_errors"] = [line for line in text.splitlines() if "[ERROR]" in line]
        stats[variant + "_streaming_incomplete"] = [line for line in text.splitlines() if "still streaming after" in line]
    reference = out / f"{scene}-reference.png"
    if reference.exists():
        r = np.asarray(Image.open(reference).convert("RGB")).astype(np.int16)
        error = np.abs(b-r)
        stats["24_vs_48_steps"] = {"mean_absolute_rgb_8bit": float(error.mean()),
            "max_rgb_8bit": int(error.max()), "p99_rgb_8bit": float(np.percentile(error,99))}
    report[scene] = stats
    width, height = images[0].size
    comparison = Image.new("RGB", (width*2, height+48), "#151a22")
    draw = ImageDraw.Draw(comparison)
    draw.text((20,8), "BEFORE / current distance haze", font=font, fill="white")
    draw.text((width+20,8), "AFTER / 24-step in-scattering preview", font=font, fill="white")
    for i,im in enumerate(images): comparison.paste(im,(i*width,48))
    comparison.save(out / f"{scene}-comparison.png")
(out / "comparison-metrics.json").write_text(json.dumps(report, indent=2))
print(json.dumps(report, indent=2))
