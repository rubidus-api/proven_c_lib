#!/bin/sh

if ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    echo "arm-none-eabi-gcc not found. Skipping freestanding cross-compile check."
    exit 0
fi

echo "Running freestanding cross-compile checks..."

CFLAGS="-mcpu=cortex-m4 -mthumb -std=c23 -ffreestanding -nostdlib -DPROVEN_FREESTANDING -DPROVEN_FMT_NO_FLOAT -DPROVEN_NO_U16STR -I include -c"
CC="arm-none-eabi-gcc"

FILES="src/proven/memory.c src/proven/arena.c src/proven/pool.c src/proven/buffer.c src/proven/heap.c src/proven/array.c src/proven/ring.c src/proven/map.c src/proven/algorithm.c src/proven/time.c src/proven/fmt.c src/proven/scan.c src/proven/panic.c src/proven/u8str.c"

mkdir -p build/cross

for file in $FILES; do
    echo "Compiling $file..."
    if ! $CC $CFLAGS $file -o build/cross/$(basename $file).o; then
        echo "Failed to compile $file for freestanding ARM."
        exit 1
    fi
done

echo "All freestanding modules compiled successfully for ARM Cortex-M4."
