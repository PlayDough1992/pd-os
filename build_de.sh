#!/bin/bash
# ============================================================================
# PD-OS  —  build_de.sh
# ============================================================================
# Compile a custom Desktop Environment to a flat binary ready for PD-OS.
#
# Usage:
#   ./build_de.sh <source.c> [output.bin]
#
#   If output.bin is omitted, the binary is placed alongside source.c
#   with the extension replaced by .bin.
#
# The resulting .bin can be installed with:
#   ./build_de.sh --install <name> <binary.bin>
#
# --install copies the binary into the disk image at /sys/de/<name>.bin
# and sets /sys/de/active to <name>.
#
# Examples:
#   ./build_de.sh sdk/example_de/example_de.c
#   ./build_de.sh sdk/example_de/example_de.c build/example_de.bin
#   ./build_de.sh --install example_de build/example_de.bin
# ============================================================================

set -e

CROSS_CC="i686-linux-gnu-gcc"
SDK_INC="sdk/include"
DISK_IMAGE="build/pd-os-gde.img"

CFLAGS="-m32 -ffreestanding -nostdlib -nostdinc -fno-builtin \
        -fno-stack-protector -nostartfiles -nodefaultlibs \
        -fno-pic -fno-pie -no-pie \
        -Wall -Wextra"

# Fixed DE load address — must match DE_LOAD_ADDR in de_api.h
DE_LOAD_ADDR="0x01000000"

# PDFS data partition starts at sector 1024 in the disk image (512-byte sectors)
PDFS_START_SECTOR=1024

GREEN='\033[0;32m'
CYAN='\033[0;36m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

# ============================================================================
# install_de  —  write DE binary into PDFS at /sys/de/<name>.bin
# and set /sys/de/active to <name>.
#
# PDFS on-disk layout is managed by the kernel's pdfs.c.  We use a Python
# helper that speaks the PDFS wire format directly to inject files without
# needing the kernel to be running.
# ============================================================================
install_de() {
    local DE_NAME="$1"
    local BIN_FILE="$2"

    if [ ! -f "$BIN_FILE" ]; then
        echo -e "${RED}[FAIL] Binary not found: $BIN_FILE${NC}"
        exit 1
    fi
    if [ ! -f "$DISK_IMAGE" ]; then
        echo -e "${RED}[FAIL] Disk image not found: $DISK_IMAGE — run ./build_gde.sh all first.${NC}"
        exit 1
    fi

    local BIN_SIZE
    BIN_SIZE=$(wc -c < "$BIN_FILE")
    if [ "$BIN_SIZE" -gt $((512 * 1024)) ]; then
        echo -e "${RED}[FAIL] Binary is $BIN_SIZE bytes — exceeds 512 KB DE limit.${NC}"
        exit 1
    fi

    echo -e "${CYAN}Installing DE '$DE_NAME' ($BIN_SIZE bytes) into $DISK_IMAGE...${NC}"

    # Use the pdfs_inject.py helper to write the file into PDFS
    python3 tools/pdfs_inject.py \
        "$DISK_IMAGE" \
        "$PDFS_START_SECTOR" \
        "/sys/de/${DE_NAME}.bin" \
        "$BIN_FILE"

    python3 tools/pdfs_inject.py \
        "$DISK_IMAGE" \
        "$PDFS_START_SECTOR" \
        "/sys/de/active" \
        <(printf '%s' "$DE_NAME")

    echo -e "${GREEN}[OK] DE installed.  Boot the image to use '$DE_NAME'.${NC}"
    echo -e "${YELLOW}Tip: qemu-system-i386 -drive format=raw,file=$DISK_IMAGE -m 128M -enable-kvm -cpu host -display sdl${NC}"
}

# ============================================================================
# build_de  —  compile a .c file to a DE flat binary
# ============================================================================
build_de() {
    local SRC="$1"
    local OUT="$2"

    if [ ! -f "$SRC" ]; then
        echo -e "${RED}[FAIL] Source file not found: $SRC${NC}"
        exit 1
    fi

    # Derive output name if not given
    if [ -z "$OUT" ]; then
        OUT="${SRC%.c}.bin"
    fi

    echo -e "${CYAN}Compiling DE: $SRC → $OUT${NC}"

    $CROSS_CC $CFLAGS \
        -I"$SDK_INC" \
        -Wl,-Ttext,$DE_LOAD_ADDR \
        -Wl,-e,de_main \
        "$SRC" \
        -o "$OUT"

    local SZ
    SZ=$(wc -c < "$OUT")
    if [ "$SZ" -gt $((512 * 1024)) ]; then
        echo -e "${RED}[FAIL] Output binary is $SZ bytes — exceeds 512 KB limit.${NC}"
        exit 1
    fi

    echo -e "${GREEN}[OK] DE binary: $OUT  ($SZ bytes)${NC}"
}

# ============================================================================
# Dispatch
# ============================================================================

if [ "$1" = "--install" ]; then
    if [ $# -lt 3 ]; then
        echo "Usage: $0 --install <name> <binary.bin>"
        exit 1
    fi
    install_de "$2" "$3"
elif [ $# -ge 1 ]; then
    build_de "$1" "$2"
else
    echo "Usage:"
    echo "  $0 <source.c> [output.bin]              Build a DE"
    echo "  $0 --install <name> <binary.bin>         Install into disk image"
    exit 1
fi
