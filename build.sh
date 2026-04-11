#!/bin/bash
# ============================================================================
# PD-OS Build Script (Linux)
# ============================================================================
# Usage: ./build.sh [all|run|run-debug|clean|setup-check]
# ============================================================================

set -e

TARGET="${1:-all}"

BUILD_DIR="build"
BOOTLOADER_SRC="bootloader/stage1.asm"
BOOTLOADER_BIN="$BUILD_DIR/bootloader.bin"
DISK_IMAGE="$BUILD_DIR/pd-os.img"
DISK_SIZE=2880   # 2880 * 512 = 1.44MB floppy

GREEN='\033[0;32m'
CYAN='\033[0;36m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

build() {
    echo -e "${CYAN}Building PD-OS...${NC}"
    mkdir -p "$BUILD_DIR"

    nasm -f bin "$BOOTLOADER_SRC" -o "$BOOTLOADER_BIN"

    SIZE=$(wc -c < "$BOOTLOADER_BIN")
    if [ "$SIZE" -ne 512 ]; then
        echo -e "${RED}[FAIL] Bootloader must be exactly 512 bytes (got $SIZE)${NC}"
        exit 1
    fi
    echo -e "${GREEN}[OK] Bootloader built: $SIZE bytes${NC}"

    # Create 1.44MB blank floppy image
    dd if=/dev/zero of="$DISK_IMAGE" bs=512 count=$DISK_SIZE 2>/dev/null
    # Write bootloader to first sector
    dd if="$BOOTLOADER_BIN" of="$DISK_IMAGE" conv=notrunc bs=512 count=1 2>/dev/null
    echo -e "${GREEN}[OK] Disk image created: $DISK_IMAGE${NC}"
}

case "$TARGET" in
    all)
        build
        echo -e "${GREEN}=== Build complete! ===${NC}"
        echo "Run with: ./build.sh run"
        ;;
    run)
        build
        echo -e "${CYAN}Starting QEMU...${NC}"
        echo "Press Ctrl+Alt+G to release mouse, Ctrl+C to quit"
        qemu-system-i386 -drive format=raw,file="$DISK_IMAGE" -m 128M
        ;;
    run-debug)
        build
        echo -e "${CYAN}Starting QEMU (debug mode)...${NC}"
        qemu-system-i386 -drive format=raw,file="$DISK_IMAGE" -m 128M \
            -serial stdio -no-reboot -d cpu_reset
        ;;
    clean)
        if [ -d "$BUILD_DIR" ]; then
            rm -rf "$BUILD_DIR"
            echo -e "${GREEN}[OK] Cleaned${NC}"
        else
            echo "Nothing to clean."
        fi
        ;;
    setup-check)
        echo "=== Verifying PD-OS Toolchain Setup ==="
        echo ""
        for tool in nasm i686-elf-gcc i686-elf-ld qemu-system-i386; do
            printf "Checking %-20s " "$tool..."
            if command -v "$tool" > /dev/null 2>&1; then
                echo -e "${GREEN}OK${NC}"
            else
                echo -e "${RED}NOT FOUND${NC}"
            fi
        done
        echo ""
        echo "If any tools are missing, run: ./tools/autosetup.sh"
        ;;
    *)
        echo "PD-OS Build System (Linux)"
        echo "Usage: ./build.sh [all|run|run-debug|clean|setup-check]"
        ;;
esac
