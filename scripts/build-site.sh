#!/bin/sh
# Build the published manual: Markdown -> Typst -> HTML pages and one PDF, per language.
#
# The Markdown under manual/ and manual-ko/ is the source of truth - the build compiles its
# code blocks, runs its examples and checks its claims. Nothing here edits it. Everything in
# docs/ is generated, and regenerating is the only way to change it.
#
#   scripts/build-site.sh            build both languages into docs/
#   scripts/build-site.sh en         build one language
#
# Typst is the workspace's shared installation; see usr/docs/typst.md. Korean text needs the
# shared font path, and Typst does NOT fail when a font is missing - it warns and substitutes,
# which is how two editions of a sibling project shipped in the wrong typeface. So the PDF step
# checks that the fonts it asked for were actually found.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
work="$root/build/site"
out="$root/docs"

typst=${TYPST:-typst}
command -v "$typst" >/dev/null 2>&1 || typst="$root/../usr/toolchains/typst/typst"
[ -x "$typst" ] || { echo "build-site: no typst binary (set TYPST)" >&2; exit 1; }
fonts=${TYPST_FONT_PATHS:-$root/../usr/toolchains/fonts}

version=$(sed -n 's/^#define PROVEN_VERSION_STRING "\(.*\)"$/\1/p' "$root/include/proven/version.h")
[ -n "$version" ] || { echo "build-site: cannot read the version" >&2; exit 1; }
# The release tag drops the project prefix the version string carries: the tags in this
# repository are v26.07.23b, not proven_c_lib-v26.07.23b. The PDF link is built from the tag,
# so getting this wrong publishes a download link that 404s.
tag=${version#proven_c_lib-}

langs=${*:-"en ko"}

page_name() {   # manual-01-foundation-ko.md -> manual-01-foundation ; manual-ko.md -> index
    n=$(basename "$1" .md)
    n=${n%-ko}
    [ "$n" = "manual" ] && n=index
    printf '%s' "$n"
}

build_lang() {
    lang=$1
    case "$lang" in
        en) src="$root/manual" ;;
        ko) src="$root/manual-ko" ;;
        *) echo "build-site: unknown language: $lang" >&2; exit 2 ;;
    esac

    echo "build-site: $lang"
    rm -rf "$work/$lang"
    mkdir -p "$work/$lang/typ" "$work/$lang/frag" "$out/$lang"

    # 1. Markdown -> Typst, one file per chapter.
    for md in "$src"/*.md; do
        name=$(page_name "$md")
        python3 "$root/scripts/md2typst.py" "$md" "$work/$lang/typ/$name.typ" "$lang"
    done

    # 2. Typst -> an HTML fragment per chapter.
    for typ in "$work/$lang/typ"/*.typ; do
        name=$(basename "$typ" .typ)
        TYPST_FONT_PATHS="$fonts" "$typst" compile --root "$root" \
            --features html --format html \
            "$typ" "$work/$lang/frag/$name.html" 2>"$work/$lang/frag/$name.log" \
            || { echo "build-site: $lang/$name failed to compile to HTML:" >&2
                 cat "$work/$lang/frag/$name.log" >&2; exit 1; }
    done

    # 3. Fragments -> published pages, with anchors, navigation, settings and the search index.
    #    The PDF link points at the release asset, not at a copy in the site: a published PDF
    #    belongs to a version, and a reader who downloads it should get the one they were
    #    reading. The local copy beside the pages is what the release uploads.
    pdf_url="https://github.com/rubidus-api/proven_c_lib/releases/download/$tag/$version-$lang-manual.pdf"
    python3 "$root/scripts/site_pages.py" "$lang" "$work/$lang/frag" "$out/$lang" "$version" "$pdf_url"
    cp -f "$root/site/manual.css" "$out/$lang/manual.css"

    # 4. One PDF per language, from the same Typst files.
    book="$work/$lang/manual.typ"
    {
        printf '#set document(title: "proven_c_lib %s", author: "proven_c_lib")\n' "$version"
        printf '#set page(paper: "a4", margin: (x: 2.2cm, y: 2.2cm), numbering: "1")\n'
        if [ "$lang" = "ko" ]; then
            printf '#set text(font: ("Noto Serif CJK KR", "Noto Serif"), size: 10pt, lang: "ko")\n'
        else
            printf '#set text(font: ("Noto Serif", "Noto Serif CJK KR"), size: 10pt, lang: "en")\n'
        fi
        printf '#show raw: set text(font: "D2Coding", size: 8.5pt)\n'
        printf '#set par(justify: false, leading: 0.72em)\n'
        printf '#show link: set text(fill: rgb("#1f5fa8"))\n'
        printf '#show heading: set block(above: 1.4em, below: 0.7em)\n'
        printf '#outline(depth: 2)\n#pagebreak()\n'
        for name in index manual-t-tutorial manual-00-start-here manual-01-foundation manual-02-allocation \
                    manual-03-strings-text manual-04-containers-algorithms manual-08-fmt-scan \
                    manual-05-hosted-services manual-06-execution-and-platform \
                    manual-freestanding manual-07-alias-xcv-index; do
            [ -f "$work/$lang/typ/$name.typ" ] || continue
            printf '#include "typ/%s.typ"\n#pagebreak()\n' "$name"
        done
    } > "$book"

    TYPST_FONT_PATHS="$fonts" "$typst" compile --root "$root" "$book" "$out/$lang/manual.pdf" \
        2>"$work/$lang/pdf.log" || { echo "build-site: $lang PDF failed:" >&2; cat "$work/$lang/pdf.log" >&2; exit 1; }

    # The font trap: Typst warns and substitutes rather than failing, so check the warning text.
    if grep -q "unknown font family" "$work/$lang/pdf.log"; then
        echo "build-site: $lang PDF was set in substitute fonts:" >&2
        grep "unknown font family" "$work/$lang/pdf.log" >&2
        exit 1
    fi
    echo "build-site: $lang PDF -> docs/$lang/manual.pdf"
}

for lang in $langs; do
    build_lang "$lang"
done

# The landing page is the front door, and it shows the whole house: the complete contents of
# both editions, every chapter and every section, so a reader lands and goes straight to the
# paragraph they want. The contents are what the per-language build just wrote.
: > "$out/.nojekyll"
python3 "$root/scripts/site_root.py" "$out" "$version" \
    "$work/en/frag/contents.json" "$work/ko/frag/contents.json"

# Every link the manual makes between chapters and to its own sections must resolve in the
# generated site. A cross-reference that works in Markdown and 404s on the web is the failure
# this check exists to catch, and it is mechanical, so the build does it.
# The fonts are subset from the characters the generated pages use, so they are built last.
sh "$root/scripts/make-webfonts.sh"

python3 "$root/scripts/check-site-links.py" "$out" || exit 1

echo "build-site: done -> docs/"
