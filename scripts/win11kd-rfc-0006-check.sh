#!/bin/sh
# Native Windows run of the RFC-0006 verifier, on the vm_win11_dev_kd test VM.
#
#   build   on arch-dev (mingw; it shares this storage, so no transfer step)
#   copy    to $PROVEN_WIN_HOST:$PROVEN_WIN_DIR/
#   run     win64, win32 and win64-legacy (acts as pre-1809 Windows), each in a FRESH subdirectory, stdin from nul
#           (the verifier waits for Enter on Windows)
#   fetch   both reports into build/rfc-0006/win11kd-<date>/ and print the verdicts
#
# Needs, from the environment (nothing machine-specific is written here):
#   PROVEN_BUILD_SSH   ssh arguments reaching the build host that shares this storage,
#                      e.g. "-i KEY -p PORT user@host"
#   PROVEN_BUILD_DIR   the repository path on that host
#   PROVEN_WIN_HOST    ssh alias of the Windows VM (default: win11kd)
#   PROVEN_WIN_DIR     working folder on the VM, forward slashes
# Traps already paid for:
#   - the VM's default shell IS cmd. Do not prefix with `cmd /c`: the outer cmd splits
#     on `&`, the `cd` does not carry over, and the rest runs in the home directory.
#   - its output is CP949; it goes through iconv.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
: "${PROVEN_BUILD_SSH:?set it - see the header}" "${PROVEN_BUILD_DIR:?}" "${PROVEN_WIN_DIR:?}"
win=${PROVEN_WIN_HOST:-win11kd}
remote_dir=$PROVEN_WIN_DIR
out="$here/build/rfc-0006/win11kd-$(date +%Y-%m-%d)"
mkdir -p "$out"

# shellcheck disable=SC2086
ssh -o BatchMode=yes $PROVEN_BUILD_SSH \
    "cd $PROVEN_BUILD_DIR && sh scripts/build-rfc-0006-check.sh >/dev/null && sha256sum dist/rfc-0006-check-win64.exe dist/rfc-0006-check-win32.exe"

scp -o BatchMode=yes -q "$here/dist/rfc-0006-check-win64.exe" "$here/dist/rfc-0006-check-win32.exe" "$here/dist/rfc-0006-check-win64-legacy.exe" \
    "$win:$remote_dir/"

for w in 64 32 64-legacy; do
    ssh -o BatchMode=yes "$win" \
        "cd /d ${remote_dir} && (if exist run$w rmdir /s /q run$w) & mkdir run$w && cd run$w && ..\\rfc-0006-check-win$w.exe < nul > nul & exit 0" \
        | iconv -f CP949 -t UTF-8 || true
    scp -o BatchMode=yes -q "$win:$remote_dir/run$w/proven-windows-check-report.txt" "$out/report-win$w.txt"
    printf 'win%s: ' "$w"
    grep -E '^checks:|^VERDICT|FAIL' "$out/report-win$w.txt" || echo "(no verdict line - read $out/report-win$w.txt)"
done
echo "reports: $out"
