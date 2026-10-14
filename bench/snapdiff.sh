#!/usr/bin/env bash
# bench/snapdiff.sh A.exe B.exe — draws the same test screen in each build and
# compares the frames pixel for pixel (bench/snapscreen.sh is what gets drawn).
cd "$(dirname "$0")/.."
B=bench/termbench.exe; T="--class hardcore --title hcbench"
snap() {
  /mnt/c/Windows/System32/taskkill.exe /F /IM hcbench.exe >/dev/null 2>&1; sleep 0.3
  cp "$1" build/hcbench/hcbench.exe
  (cd /mnt/c && setsid "$OLDPWD/build/hcbench/hcbench.exe" --no-activate --cd "$(wslpath -w "$OLDPWD/build/hcbench")" </dev/null >/dev/null 2>&1 &)
  for i in $(seq 100); do $B cmd "tab 0" $T >/dev/null 2>&1 && break; sleep 0.1; done
  $B resize 1200,700 $T; sleep 2.5
  $B send "clear; bash $PWD/bench/snapscreen.sh\n" $T; sleep 1.5
  $B cmd "tophover 1" $T; $B cmd "hover 40,20" $T; $B cmd "sel ${SEL:-1 3 4 70}" $T
  $B snap "$(wslpath -w "$PWD/$2")" $T; sleep 0.8
  /mnt/c/Windows/System32/taskkill.exe /F /IM hcbench.exe >/dev/null 2>&1
}
mkdir -p build/hcbench
snap "$1" build/snap-a.bmp; snap "$2" build/snap-b.bmp
python3 - <<'PY'
from PIL import Image, ImageChops
a = Image.open("build/snap-a.bmp").convert("RGB"); b = Image.open("build/snap-b.bmp").convert("RGB")
if a.size != b.size: print("size differs", a.size, b.size); raise SystemExit(1)
d = ImageChops.difference(a, b); box = d.getbbox()
n = sum(1 for p in d.getdata() if p != (0, 0, 0))
print("identical" if not box else "%d pixels differ, in %s" % (n, box))
b.save("build/snap-b.png"); d.point(lambda v: 255 if v else 0).save("build/snap-diff.png")
PY
