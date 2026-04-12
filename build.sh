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
DISK_SIZE=2880   # 2880 * 512 = 1.44 MB

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
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/core/users.c"           -o "$BUILD_DIR/users.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/core/login.c"           -o "$BUILD_DIR/login.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/mm/e820.c"              -o "$BUILD_DIR/e820.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/mm/pmm.c"               -o "$BUILD_DIR/pmm.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/mm/paging.c"            -o "$BUILD_DIR/paging.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/mm/kheap.c"             -o "$BUILD_DIR/kheap.o"
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
        "$BUILD_DIR/users.o" \
        "$BUILD_DIR/login.o" \
        "$BUILD_DIR/e820.o" \
        "$BUILD_DIR/pmm.o" \
        "$BUILD_DIR/paging.o" \
        "$BUILD_DIR/kheap.o" \
        "$BUILD_DIR/shell.o" \
        "$BUILD_DIR/kernel_main.o" \
        -o "$KERNEL_ELF"

    $CROSS_OBJCOPY -O binary "$KERNEL_ELF" "$KERNEL_BIN"
    KSIZE=$(wc -c < "$KERNEL_BIN")
    echo -e "${GREEN}  [OK] Kernel: $KSIZE bytes${NC}"

    # --- Disk image ---
    echo -e "${CYAN}  [5] Creating disk image...${NC}"
    dd if=/dev/zero           of="$DISK_IMAGE" bs=512 count=$DISK_SIZE 2>/dev/null
    dd if="$BOOTLOADER_BIN"  of="$DISK_IMAGE" conv=notrunc bs=512 seek=0 count=1  2>/dev/null
    dd if="$STAGE2_BIN"      of="$DISK_IMAGE" conv=notrunc bs=512 seek=1          2>/dev/null
    dd if="$KERNEL_BIN"      of="$DISK_IMAGE" conv=notrunc bs=512 seek=6          2>/dev/null
    echo -e "${GREEN}  [OK] Disk image: $DISK_IMAGE${NC}"
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
