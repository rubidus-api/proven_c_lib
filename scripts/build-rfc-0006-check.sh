#!/bin/sh
# Builds docs/rfc-0006-runtime-check.c for the targets a person can run it on, into dist/.
#
# RFC-0006 H-005 and H-006 are Windows defects. They were fixed by inspection and they
# cross-compile, and neither of those is a runtime result: nothing in this project has ever
# executed a Windows binary. docs/BACKLOG.md B-033 stays open for that reason. This builds
# the program that closes it - somebody runs it on Windows and sends the output back.
#
# The Linux build is not a courtesy. A verifier nobody has executed is not a verifier, and
# running it here is how the harness itself gets checked before it is handed to someone.
#
# Usage: scripts/build-rfc-0006-check.sh        (from the repository root)
set -eu

SRC="docs/rfc-0006-runtime-check.c"
OUT="dist"
CFLAGS="-std=c2x -Wall -Wextra -O2 -I./include -I./platform"

[ -f "$SRC" ] || { echo "run this from the repository root" >&2; exit 2; }
mkdir -p "$OUT"

built=""
skipped=""

# Windows, 64-bit and 32-bit. -static so the person running it needs no mingw runtime DLLs
# beside the executable; -lbcrypt is the CSPRNG the entropy path calls.
for pair in "x86_64-w64-mingw32-gcc rfc-0006-check-win64.exe" \
            "i686-w64-mingw32-gcc rfc-0006-check-win32.exe"; do
    cc_exe=${pair%% *}
    out_name=${pair##* }
    if command -v "$cc_exe" >/dev/null 2>&1; then
        # shellcheck disable=SC2086
        "$cc_exe" $CFLAGS -static -Wl,--no-insert-timestamp -o "$OUT/$out_name" \
            "$SRC" src/proven/*.c platform/*.c -lbcrypt
        built="$built $out_name"
    else
        skipped="$skipped $out_name($cc_exe)"
    fi
done

# The host, so the harness can be run before it is trusted.
if command -v cc >/dev/null 2>&1; then
    # shellcheck disable=SC2086
    cc $CFLAGS -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -o "$OUT/rfc-0006-check-posix" \
        "$SRC" src/proven/*.c platform/*.c -pthread -lm
    built="$built rfc-0006-check-posix"
fi

echo "built:$built"
[ -n "$skipped" ] && echo "skipped (no compiler):$skipped"
echo "dist/ now holds:"
ls -la "$OUT" | grep rfc-0006 || true
echo
echo "Hand the Windows executable to someone with a Windows machine. It writes"
echo "proven-windows-check-report.txt beside itself; that file is the evidence."
