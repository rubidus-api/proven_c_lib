#!/bin/sh
set -e

# Setup basics
COMPILER="gcc"

if [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    echo "Usage: ./build.sh [COMPILER] [OPTIONS...]"
    echo "  COMPILER : gcc (default), clang, icx, cc, tcc"
    echo "  OPTIONS  : Flags applied directly to compilation (e.g. -O3, -Wall)"
    echo "Examples :"
    echo "  ./build.sh                  (Uses gcc)"
    echo "  ./build.sh clang -O2 -g     (Uses clang with flags)"
    exit 0
fi

# Determine compiler from first argument if matched
case "$1" in
    gcc|clang|icx|cc|tcc)
        COMPILER="$1"
        shift
        ;;
esac

# Export the chosen setup so nob.c can read it to build the actual library
export NOB_COMPILER="$COMPILER"
export CC="$COMPILER"

echo "=> 1. Compiling build script (nob.c) with $COMPILER..."
$COMPILER -o nob nob.c

echo "=> 2. Running build script with custom options..."
./nob "$@"
