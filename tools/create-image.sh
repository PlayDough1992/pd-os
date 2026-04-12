#!/bin/bash
# ============================================================================
# PD-OS Disk Image Builder (Linux)
# ============================================================================
# Creates a bootable floppy disk image from the compiled bootloader.
# Usage: ./tools/create-image.sh [bootloader_path] [output_image] [size_kb]
# ============================================================================

set -e

BOOTLOADER_PATH="${1:-build/bootloader.bin}"
OUTPUT_IMAGE="${2:-build/pd-os.img}"
DISK_SIZE_SECTORS=2880   # 2880 * 512 = 1.44MB

GREEN='\033[0;32m'
CYAN='\033[0;36m'
RED='\033[0;31m'
NC='\033[0m'

echo -e "${CYAN}=== PD-OS Disk Image Builder ===${NC}"
echo ""

# Check bootloader exists
if [ ! -f "$BOOTLOADER_PATH" ]; then
    echo -e "${RED}Error: Bootloader not found at $BOOTLOADER_PATH${NC}"
    echo "Build the bootloader first: make bootloader"
    exit 1
fi

# Verify exactly 512 bytes
SIZE=$(wc -c < "$BOOTLOADER_PATH")
if [ "$SIZE" -ne 512 ]; then
    echo -e "${RED}Error: Bootloader must be exactly 512 bytes (got $SIZE bytes)${NC}"
    exit 1
fi
echo -e "${GREEN}[OK] Bootloader verified: $SIZE bytes${NC}"

# Create output directory if needed
mkdir -p "$(dirname "$OUTPUT_IMAGE")"

# Create blank 1.44MB floppy image
echo -e "${CYAN}Creating disk image: $OUTPUT_IMAGE ($(( DISK_SIZE_SECTORS / 2 )) KB)...${NC}"
dd if=/dev/zero of="$OUTPUT_IMAGE" bs=512 count=$DISK_SIZE_SECTORS 2>/dev/null

# Write bootloader to first sector
dd if="$BOOTLOADER_PATH" of="$OUTPUT_IMAGE" conv=notrunc bs=512 count=1 2>/dev/null

echo -e "${GREEN}[OK] Disk image created: $OUTPUT_IMAGE${NC}"
echo ""
echo "Run with QEMU:"
echo "  qemu-system-i386 -drive format=raw,file=$OUTPUT_IMAGE -m 128M"
echo "  (or: make run)"
