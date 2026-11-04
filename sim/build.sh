#!/bin/bash
# One-shot compile and run of the headless BASHCAT renderer.
set -euo pipefail

SIM="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SIM/.." && pwd)"
NPROC="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
STB="$SIM/third_party/stb_image_write.h"

# LVGL 8.4.0 comes from the ESP Component Registry (run `idf.py reconfigure` once),
# or point LVGL_DIR at any LVGL v8.4.0 checkout.
LVGL="${LVGL_DIR:-$ROOT/managed_components/lvgl__lvgl}"
[[ -d "$LVGL/src" ]] || LVGL="$ROOT/components/lvgl__lvgl"
if [[ ! -d "$LVGL/src" ]]; then
  echo "LVGL not found. Run 'idf.py reconfigure' first or set LVGL_DIR." >&2
  exit 1
fi

mkdir -p "$SIM/third_party" "$SIM/build/obj" "$SIM/out/seq"

if [[ ! -f "$STB" ]]; then
  curl -fsSL -o "$STB" https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h
fi

CFLAGS="-O2 -std=c11 -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter"
CFLAGS="$CFLAGS -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE"
CFLAGS="$CFLAGS -DCONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=1"
CFLAGS="$CFLAGS -I$SIM -I$SIM/include -I$SIM/third_party -I$ROOT/main/ui -I$ROOT/main/io -I$LVGL"

mf="$SIM/build/build.mk"
list="$SIM/build/sources.txt"
{
  find "$LVGL/src" -name '*.c'
  printf '%s\n' "$SIM/main.c" "$SIM/port.c" "$SIM/io_stub.c" \
    "$ROOT/main/ui/bashcat_ui.c" "$ROOT/main/ui/bashcat_ui_v3.c"
} | sort > "$list"

{
  echo "CC := cc"
  echo "CFLAGS := $CFLAGS"
  echo
  objs=""
  while IFS= read -r src; do
    rel="${src#"$ROOT/"}"
    safe="$(printf '%s' "$rel" | tr '/.' '__')"
    obj="$SIM/build/obj/${safe}.o"
    objs="$objs $obj"
    printf '%s: %s %s/lv_conf.h\n' "$obj" "$src" "$SIM"
    printf '\t@mkdir -p %s\n' "$SIM/build/obj"
    printf '\t$(CC) $(CFLAGS) -c -o %s %s\n\n' "$obj" "$src"
  done < "$list"
  echo "all: $SIM/build/bashcat_sim"
  echo "$SIM/build/bashcat_sim:$objs"
  printf '\t$(CC) -o $@ %s -lm\n' "$objs"
} > "$mf"

make -f "$mf" -j"$NPROC" "$SIM/build/bashcat_sim"

cd "$SIM"
rm -rf out/seq out/v3seq out/boot.mp4 out/boot.gif out/v3.gif
rm -f out/frame_*.png out/v3_*.png
mkdir -p out/seq out/v3seq
./build/bashcat_sim

if command -v ffmpeg >/dev/null 2>&1; then
  ffmpeg -y -loglevel error -framerate 25 -i out/seq/frame_%05d.png \
    -c:v libx264 -pix_fmt yuv420p out/boot.mp4
  ffmpeg -y -loglevel error -framerate 25 -i out/seq/frame_%05d.png \
    -vf "scale=400:-1:flags=lanczos,split[s0][s1];[s0]palettegen=stats_mode=diff[p];[s1][p]paletteuse" \
    out/boot.gif
  ffmpeg -y -loglevel error -framerate 10 -i out/v3seq/frame_%05d.png \
    -vf "scale=400:-1:flags=lanczos,split[s0][s1];[s0]palettegen=stats_mode=diff[p];[s1][p]paletteuse" \
    out/v3.gif
fi

python3 - <<'PY'
import os
from pathlib import Path
try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    raise SystemExit(0)
sim = Path(os.environ.get("SIM_DIR", "."))
ref = sim / "ref"
out = sim / "out"
pairs = [
    ("v3_05000ms_dash.png", "dash"),
    ("frame_12000ms.png", "late_dash"),
    ("v3_13600ms_io.png", "io"),
    ("v3_14200ms_do_high.png", "do_high"),
    ("v3_16600ms_i2c.png", "i2c"),
    ("v3_18600ms_scan_done.png", "scan_done"),
    ("v3_21550ms_can_running.png", "can"),
    ("v3_24650ms_ping_done.png", "rs485"),
    ("v3_26100ms_system.png", "system"),
]
for name, tag in pairs:
    a = ref / name
    b = out / name
    if not a.exists() or not b.exists():
        print(f"perf_compare skip {tag}")
        continue
    ia = Image.open(a).convert("RGB")
    ib = Image.open(b).convert("RGB")
    gap = 8
    canvas = Image.new("RGB", (ia.width + gap + ib.width, ia.height + 28), (11, 13, 15))
    canvas.paste(ia, (0, 28))
    canvas.paste(ib, (ia.width + gap, 28))
    draw = ImageDraw.Draw(canvas)
    draw.text((8, 6), f"before  {tag}", fill=(227, 231, 234))
    draw.text((ia.width + gap + 8, 6), "after", fill=(227, 231, 234))
    dest = out / f"perf_compare_{tag}.png"
    canvas.save(dest)
    diff = 0
    pa, pb = ia.load(), ib.load()
    for y in range(ia.height):
        for x in range(ia.width):
            if pa[x, y] != pb[x, y]:
                diff += 1
    print(f"perf_compare {tag} diff_px={diff} -> {dest.name}")
PY
