#!/usr/bin/env bash
# Build the web fonts the published manual ships, subset to the characters it actually uses.
#
# Why this exists: without an @font-face rule the stylesheet only *names* a font, so a visitor
# who does not have it installed gets a substitute. `Noto Sans CJK KR` is rare outside Linux
# desktops, and the substitutes for Korean are Chinese and Japanese faces that look almost
# right - which is how a sibling project published two editions in the wrong typeface before
# anyone noticed. So the fonts are shipped, and subsetting keeps that affordable: only the
# characters this site uses survive, which takes a Korean face from megabytes to ~200 KB.
#
# The characters come from the generated HTML, so this runs AFTER the pages are built.
#
# Needs fonttools + brotli; a virtual environment is made if they are missing.
#
#   scripts/make-webfonts.sh
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
fonts=${TYPST_FONT_PATHS:-$root/../usr/toolchains/fonts}
venv=${WEBFONT_VENV:-$root/build/fontvenv}

if [ ! -x "$venv/bin/pyftsubset" ]; then
  echo "make-webfonts: preparing the tools in $venv"
  python3 -m venv "$venv"
  "$venv/bin/pip" install --quiet fonttools brotli
fi

chars="$root/build/site/used-chars.txt"
python3 - "$root/docs" "$chars" <<'PY'
import html, pathlib, re, sys
docs, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
chars = set()
for lang in ("ko", "en"):
    d = docs / lang
    if not d.exists():
        continue
    for f in d.glob("*.html"):
        chars.update(html.unescape(re.sub(r"<[^>]+>", " ", f.read_text(encoding="utf-8"))))
chars = {c for c in chars if ord(c) >= 0x20}
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text("".join(sorted(chars)), encoding="utf-8")
print("make-webfonts: %d character(s) used by the site" % len(chars))
PY

sub() {                      # sub <output name> <source font> <out dir>
  [ -f "$2" ] || { echo "  missing font: $2" >&2; return 1; }
  "$venv/bin/pyftsubset" "$2" --text-file="$chars" --flavor=woff2 \
      --layout-features='' --no-hinting --desubroutinize \
      --output-file="$3/$1.woff2"
}

# Both language trees get their own copy, because the stylesheet sits beside them and asks for
# `fonts/…` relative to itself. Two copies of ~400 KB is cheaper than a shared path that breaks
# the day one language moves.
for lang in en ko; do
  out="$root/docs/$lang/fonts"
  [ -d "$root/docs/$lang" ] || continue
  mkdir -p "$out"
  sub sans        "$fonts/noto-latin/NotoSans-Regular.ttf"                  "$out"
  sub sans-bold   "$fonts/noto-latin/NotoSans-Bold.ttf"                     "$out"
  sub sans-kr     "$fonts/noto-cjk-kr/NotoSansCJKkr-Regular.otf"            "$out"
  sub sans-kr-bold "$fonts/noto-cjk-kr/NotoSansCJKkr-Bold.otf"              "$out"
  sub serif       "$fonts/noto-latin/NotoSerif-Regular.ttf"                 "$out"
  sub serif-italic "$fonts/noto-latin/NotoSerif-Italic.ttf"                 "$out"
  sub mono        "$fonts/d2coding/D2Coding-Ver1.3.2-20180524.ttf"          "$out"
  sub mono-bold   "$fonts/d2coding/D2CodingBold-Ver1.3.2-20180524.ttf"      "$out"

  # Noto and D2Coding are both SIL Open Font License 1.1, which permits subsetting and
  # redistribution on the condition that the licence travels with the files.
  cat > "$out/README.md" <<'EOF'
# Fonts shipped with the web manual

The `.woff2` files here are the fonts below, subset to the characters this manual uses.
`scripts/make-webfonts.sh` generates them; do not edit them by hand.

| File | Source | Licence |
|---|---|---|
| `sans*`, `serif*` | Noto Sans / Noto Serif (Google) | SIL Open Font License 1.1 |
| `sans-kr*` | Noto Sans CJK KR (Google) | SIL Open Font License 1.1 |
| `mono*` | D2Coding (NAVER) | SIL Open Font License 1.1 |

All three are OFL 1.1, which permits subsetting and redistribution. The full text travels with
each upstream distribution — Noto: `github.com/notofonts`, D2Coding:
`github.com/naver/d2codingfont`.
EOF
  echo "make-webfonts: $lang -> $(ls "$out"/*.woff2 | wc -l) file(s), $(du -sh "$out" | cut -f1)"
done
