#!/bin/bash
# ============================================================================
# PD-OS Build Script (Linux)
# ============================================================================
# Usage: ./build.sh [all|run|run-debug|clean|setup-check]
# ============================================================================

set -e

TARGET="${1:-all}"

BUILD_DIR="build"

# Bootloader
BOOTLOADER_SRC="bootloader/stage1.asm"
BOOTLOADER_BIN="$BUILD_DIR/bootloader.bin"
STAGE2_SRC="bootloader/stage2.asm"
STAGE2_BIN="$BUILD_DIR/stage2.bin"

# Kernel
KERNEL_DIR="kernel"
KERNEL_ELF="$BUILD_DIR/kernel.elf"
KERNEL_BIN="$BUILD_DIR/kernel.bin"

# Disk image
DISK_IMAGE="$BUILD_DIR/pd-os.img"
DISK_SIZE=131072  # 131072 * 512 = 64 MB

# Cross-toolchain (i686-linux-gnu provided by gcc-i686-linux-gnu package)
CROSS_CC="i686-linux-gnu-gcc"
CROSS_LD="i686-linux-gnu-ld"
CROSS_OBJCOPY="i686-linux-gnu-objcopy"

# Compiler / linker flags
CFLAGS="-m32 -ffreestanding -nostdlib -nostdinc -fno-builtin \
        -fno-stack-protector -nostartfiles -nodefaultlibs \
        -fno-pic -fno-pie \
        -Wall -Wextra -c"
IFLAGS="-I$KERNEL_DIR/include"
LDFLAGS="-m elf_i386 -nostdlib"

GREEN='\033[0;32m'
CYAN='\033[0;36m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

# ---- build ----------------------------------------------------------------
build() {
    echo -e "${CYAN}Building PD-OS...${NC}"
    mkdir -p "$BUILD_DIR"

    # --- Stage 1 ---
    echo -e "${CYAN}  [1] Assembling Stage 1 bootloader...${NC}"
    nasm -f bin "$BOOTLOADER_SRC" -o "$BOOTLOADER_BIN"
    SIZE=$(wc -c < "$BOOTLOADER_BIN")
    if [ "$SIZE" -ne 512 ]; then
        echo -e "${RED}[FAIL] Stage 1 must be exactly 512 bytes (got $SIZE)${NC}"
        exit 1
    fi
    echo -e "${GREEN}  [OK] Stage 1: $SIZE bytes${NC}"

    # --- Stage 2 ---
    echo -e "${CYAN}  [2] Assembling Stage 2 bootloader...${NC}"
    nasm -f bin "$STAGE2_SRC" -o "$STAGE2_BIN"
    S2SIZE=$(wc -c < "$STAGE2_BIN")
    echo -e "${GREEN}  [OK] Stage 2: $S2SIZE bytes${NC}"

    # --- Kernel ---
    echo -e "${CYAN}  [3] Compiling kernel...${NC}"

    nasm -f elf32 "$KERNEL_DIR/arch/x86/entry.asm" -o "$BUILD_DIR/entry.o"
    nasm -f elf32 "$KERNEL_DIR/arch/x86/idt.asm"   -o "$BUILD_DIR/idt_stubs.o"

    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/drivers/vga.c"          -o "$BUILD_DIR/vga.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/core/io.c"              -o "$BUILD_DIR/io.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/core/panic.c"           -o "$BUILD_DIR/panic.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/arch/x86/idt.c"         -o "$BUILD_DIR/idt.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/arch/x86/pic.c"         -o "$BUILD_DIR/pic.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/arch/x86/exceptions.c"  -o "$BUILD_DIR/exceptions.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/drivers/pit.c"          -o "$BUILD_DIR/pit.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/drivers/keyboard.c"     -o "$BUILD_DIR/keyboard.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/drivers/ata.c"           -o "$BUILD_DIR/ata.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/core/users.c"           -o "$BUILD_DIR/users.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/core/login.c"           -o "$BUILD_DIR/login.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/mm/e820.c"              -o "$BUILD_DIR/e820.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/mm/pmm.c"               -o "$BUILD_DIR/pmm.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/mm/paging.c"            -o "$BUILD_DIR/paging.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/mm/kheap.c"             -o "$BUILD_DIR/kheap.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/fs/vfs.c"               -o "$BUILD_DIR/vfs.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/fs/pdfs.c"              -o "$BUILD_DIR/pdfs.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/fs/fat32.c"             -o "$BUILD_DIR/fat32.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/core/shell.c"            -o "$BUILD_DIR/shell.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/core/kernel.c"          -o "$BUILD_DIR/kernel_main.o"

    echo -e "${CYAN}  [4] Linking kernel...${NC}"
    $CROSS_LD $LDFLAGS -T "$KERNEL_DIR/linker.ld" \
        "$BUILD_DIR/entry.o" \
        "$BUILD_DIR/idt_stubs.o" \
        "$BUILD_DIR/vga.o" \
        "$BUILD_DIR/io.o" \
        "$BUILD_DIR/panic.o" \
        "$BUILD_DIR/idt.o" \
        "$BUILD_DIR/pic.o" \
        "$BUILD_DIR/exceptions.o" \
        "$BUILD_DIR/pit.o" \
        "$BUILD_DIR/keyboard.o" \
        "$BUILD_DIR/ata.o" \
        "$BUILD_DIR/users.o" \
        "$BUILD_DIR/login.o" \
        "$BUILD_DIR/e820.o" \
        "$BUILD_DIR/pmm.o" \
        "$BUILD_DIR/paging.o" \
        "$BUILD_DIR/kheap.o" \
        "$BUILD_DIR/vfs.o" \
        "$BUILD_DIR/pdfs.o" \
        "$BUILD_DIR/fat32.o" \
        "$BUILD_DIR/shell.o" \
        "$BUILD_DIR/kernel_main.o" \
        -o "$KERNEL_ELF"

    $CROSS_OBJCOPY -O binary "$KERNEL_ELF" "$KERNEL_BIN"
    KSIZE=$(wc -c < "$KERNEL_BIN")
    echo -e "${GREEN}  [OK] Kernel: $KSIZE bytes${NC}"

    # --- Disk image ---
    echo -e "${CYAN}  [5] Creating disk image...${NC}"
    rm -f "$DISK_IMAGE"
    truncate -s $((DISK_SIZE * 512)) "$DISK_IMAGE"
    dd if="$BOOTLOADER_BIN"  of="$DISK_IMAGE" conv=notrunc bs=512 seek=0 count=1  2>/dev/null
    dd if="$STAGE2_BIN"      of="$DISK_IMAGE" conv=notrunc bs=512 seek=1          2>/dev/null
    dd if="$KERNEL_BIN"      of="$DISK_IMAGE" conv=notrunc bs=512 seek=6          2>/dev/null
    echo -e "${GREEN}  [OK] Disk image: $DISK_IMAGE (64 MB)${NC}"

    # --- PDFS init at LBA 69 ---
    # Superblock (512 bytes): magic=PDFS, version=1, dir_lba=70, dir_sectors=2,
    #                         data_lba=72, next_free_lba=72, then zeros to 512
    # Directory (1024 bytes): 32 zeroed 32-byte dirents (all free)
    echo -e "${CYAN}  [6] Initialising PDFS at LBA 200...${NC}"
    python3 -c "
import struct, sys
sb = struct.pack('<IIIIII', 0x50444653, 1, 201, 2, 203, 203)
sb += b'\\x00' * (512 - len(sb))
sys.stdout.buffer.write(sb + b'\\x00' * 1024)
" | dd of="$DISK_IMAGE" conv=notrunc bs=512 seek=200 2>/dev/null
    echo -e "${GREEN}  [OK] PDFS ready  (LBA 200-202, data from LBA 203)${NC}"
    # --- FAT32 init at LBA 2048 ---
    # Volume:  129024 sectors (LBA 2048-131071)
    # Layout:  32 reserved | FAT1(1000) | FAT2(1000) | data(127024)
    # Cluster: 1 sector/cluster (512 B), root dir = cluster 2
    echo -e "${CYAN}  [7] Formatting FAT32 at LBA 2048...${NC}"
    python3 -c "
import struct, sys
TOTAL = 129024; RES = 32; FATC = 2; FATSZ = 1000; SPC = 1
VOL_LBA = 2048; ROOT_CLUS = 2
# BPB
bpb = bytearray(512)
bpb[0:3]   = b'\\xEB\\x58\\x90'
bpb[3:11]  = b'MSDOS5.0'
struct.pack_into('<H', bpb, 11, 512)
bpb[13] = SPC
struct.pack_into('<H', bpb, 14, RES)
bpb[16] = FATC
struct.pack_into('<H', bpb, 17, 0)
struct.pack_into('<H', bpb, 19, 0)
bpb[21] = 0xF8
struct.pack_into('<H', bpb, 22, 0)
struct.pack_into('<H', bpb, 24, 63)
struct.pack_into('<H', bpb, 26, 255)
struct.pack_into('<I', bpb, 28, VOL_LBA)
struct.pack_into('<I', bpb, 32, TOTAL)
struct.pack_into('<I', bpb, 36, FATSZ)
struct.pack_into('<H', bpb, 40, 0)
struct.pack_into('<H', bpb, 42, 0)
struct.pack_into('<I', bpb, 44, ROOT_CLUS)
struct.pack_into('<H', bpb, 48, 1)
struct.pack_into('<H', bpb, 50, 6)
bpb[52:64] = b'\\x00' * 12
bpb[64]  = 0x80
bpb[65]  = 0x00
bpb[66]  = 0x29
struct.pack_into('<I', bpb, 67, 0x50444F53)
bpb[71:82] = b'PD-OS      '
bpb[82:90] = b'FAT32   '
struct.pack_into('<H', bpb, 510, 0xAA55)
# FSInfo
fsi = bytearray(512)
struct.pack_into('<I', fsi,   0, 0x41615252)
struct.pack_into('<I', fsi, 484, 0x61417272)
struct.pack_into('<I', fsi, 488, 0xFFFFFFFF)
struct.pack_into('<I', fsi, 492, 0xFFFFFFFF)
struct.pack_into('<H', fsi, 510, 0xAA55)
# FAT (both copies): reserve entries 0-1, root dir at cluster 2 = EOC
fat = bytearray(FATSZ * 512)
struct.pack_into('<I', fat,  0, 0x0FFFFFF8)
struct.pack_into('<I', fat,  4, 0x0FFFFFFF)
struct.pack_into('<I', fat,  8, 0x0FFFFFFF)
# Assemble: BPB + FSInfo + reserved[2..31] + FAT1 + FAT2 + root-dir cluster
out = bpb + fsi + bytes((RES-2)*512) + bytes(fat) + bytes(fat) + bytes(SPC*512)
sys.stdout.buffer.write(out)
" | dd of="$DISK_IMAGE" conv=notrunc bs=512 seek=2048 2>/dev/null
    echo -e "${GREEN}  [OK] FAT32 ready (LBA 2048-131071, root cluster at LBA 4080)${NC}"
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
        for tool in nasm i686-linux-gnu-gcc i686-linux-gnu-ld i686-linux-gnu-objcopy qemu-system-i386; do
            printf "Checking %-30s " "$tool..."
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
