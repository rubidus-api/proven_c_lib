#!/bin/sh
# Cut a release: run every gate, build the artefacts, and publish them to GitHub.
#
#   scripts/release.sh              build the artefacts into dist/ and stop
#   scripts/release.sh --publish    also create the GitHub release and upload them
#
# The order is deliberate: nothing is built until every gate has passed, because a release
# that ships with a failing gate is how a documentation defect reaches a reader. The gates are
# the same ones the build runs - there is no separate, weaker release standard.
#
# Publishing needs a credential, and this file never names one. Point GH_TOKEN_FILE at it:
#
#   GH_TOKEN_FILE=/path/to/credential scripts/release.sh --publish
#
# There is deliberately no default. A default is a filename, a filename in a public file is a
# map to the credential, and the project's own privacy gate refuses it - correctly.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
publish=no
[ "${1:-}" = "--publish" ] && publish=yes

version=$(sed -n 's/^#define PROVEN_VERSION_STRING "\(.*\)"$/\1/p' "$root/include/proven/version.h")
[ -n "$version" ] || { echo "release: cannot read the version" >&2; exit 1; }
tag=${version#proven_c_lib-}          # the tags here are v26.09.02a, not proven_c_lib-v26...
repo=rubidus-api/proven_c_lib

echo "release: $version (tag $tag)"

# ── the gates ──────────────────────────────────────────────────────────────────
# The full suite: every test, every manual example compiled and run, and every documentation
# gate - the version string agreeing with itself among them, so a release cannot be cut with
# the number disagreeing across the files a downstream project reads.
( cd "$root" && cc nob.c -o nob && ./nob build ) >/dev/null 2>&1 || {
    echo "release: ./nob build failed - not releasing" >&2; exit 1; }
echo "release: build and every gate passed"

( cd "$root" && ./nob asan ) >/dev/null 2>&1 || {
    echo "release: ./nob asan failed - not releasing" >&2; exit 1; }
echo "release: asan passed"

"$root/scripts/project-check.sh" >/dev/null || {
    echo "release: project-check failed (privacy or docs) - not releasing" >&2; exit 1; }
echo "release: project-check passed"

# The site is part of what is released: the PDFs come from it, and its links must resolve
# before the pages that carry them are published.
sh "$root/scripts/build-site.sh" >/dev/null || {
    echo "release: the site did not build - not releasing" >&2; exit 1; }
python3 "$root/scripts/check-site-links.py" "$root/docs" >/dev/null || {
    echo "release: the site has broken links - not releasing" >&2; exit 1; }
echo "release: site built, every internal link resolves"

# ── the artefacts ──────────────────────────────────────────────────────────────
dist="$root/dist"
mkdir -p "$dist"
for lang in en ko; do
    cp "$root/docs/$lang/manual.pdf" "$dist/$version-$lang-manual.pdf"
done

# The source archive, matching what earlier releases shipped: the library, its headers, its
# tests and its manual - not the generated site, and not the build directory.
stage="$root/build/release/$version"
rm -rf "$root/build/release"; mkdir -p "$stage"
for item in include src platform tests manual manual-ko scripts site nob.c nob.h \
            build_headers.inc build_tests.inc README.md README-ko.md LICENSE \
            THIRD_PARTY_NOTICES.md CHANGELOG.md TEST.md; do
    [ -e "$root/$item" ] && cp -r "$root/$item" "$stage/"
done
( cd "$root/build/release" && zip -qr "$dist/$version.zip" "$version" )
rm -rf "$root/build/release"

ls -la "$dist"/$version* | awk '{print "  " $9 "  " $5 " bytes"}'
[ "$publish" = "yes" ] || { echo "release: artefacts in dist/ (pass --publish to upload)"; exit 0; }

# ── publish ────────────────────────────────────────────────────────────────────
tokfile=${GH_TOKEN_FILE:-}
[ -n "$tokfile" ] && [ -f "$tokfile" ] || {
    echo "release: set GH_TOKEN_FILE to the file holding the GitHub token" >&2; exit 1; }
tok=$(tr -d '\n\r ' < "$tokfile")
api() { curl -sS -H "Authorization: token $tok" -H "Accept: application/vnd.github+json" "$@"; }

# The tag is made here rather than by the API, so the release always points at a commit that
# exists locally and has passed the gates above.
if ! git -C "$root" rev-parse "$tag" >/dev/null 2>&1; then
    git -C "$root" tag -a "$tag" -m "$version"
fi
git -C "$root" -c credential.helper="!f(){ echo username=x-access-token; echo \"password=\$(cat '$tokfile')\"; };f" \
    push origin "refs/tags/$tag" >/dev/null 2>&1 || true

notes_file="$root/build/release-notes.md"
python3 - "$root/CHANGELOG.md" "$version" > "$notes_file" <<'PY'
import re, sys
text = open(sys.argv[1], encoding='utf-8').read()
version = sys.argv[2]
# The release notes are the changelog entry for this version - one source, not two.
m = re.search(r'\n## \[[^\]]*\][^\n]*' + re.escape(version) + r'[^\n]*\n(.*?)(?=\n## \[)', text, re.S)
print((m.group(1).strip() if m else 'See CHANGELOG.md.'))
PY

existing=$(api "https://api.github.com/repos/$repo/releases/tags/$tag" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('id') or '')")
if [ -n "$existing" ]; then
    echo "release: the release for $tag already exists (id $existing)"
    rel_id=$existing
else
    rel_id=$(python3 - "$notes_file" "$tag" "$version" <<'PY' | api -X POST "https://api.github.com/repos/$repo/releases" -d @- | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('id') or ''); sys.stderr.write(str(d.get('message',''))+'\n')"
import json, sys
notes = open(sys.argv[1], encoding='utf-8').read()
print(json.dumps({"tag_name": sys.argv[2], "name": sys.argv[3], "body": notes,
                  "draft": False, "prerelease": False}))
PY
)
    [ -n "$rel_id" ] || { echo "release: could not create the release" >&2; exit 1; }
    echo "release: created release $rel_id"
fi

for f in "$dist/$version-en-manual.pdf" "$dist/$version-ko-manual.pdf" "$dist/$version.zip"; do
    name=$(basename "$f")
    curl -sS -X POST -H "Authorization: token $tok" \
         -H "Content-Type: application/octet-stream" \
         --data-binary @"$f" \
         "https://uploads.github.com/repos/$repo/releases/$rel_id/assets?name=$name" \
         -o /dev/null -w "  uploaded $name (%{http_code})\n"
done

echo "release: https://github.com/$repo/releases/tag/$tag"
