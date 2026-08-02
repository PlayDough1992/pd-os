#!/bin/bash
# ============================================================================
# PD-OS GDE Build Script
# ============================================================================
# Produces build/pd-os-gde.img  —  boots straight into the graphical desktop.
# The CLI build (build.sh) is unaffected.
# Usage: ./build_gde.sh [all|run|run-debug|clean]
# ============================================================================

set -e

TARGET="${1:-all}"

BUILD_DIR="build"

BOOTLOADER_SRC="bootloader/stage1.asm"
BOOTLOADER_BIN="$BUILD_DIR/bootloader.bin"
STAGE2_SRC="bootloader/stage2.asm"
STAGE2_BIN="$BUILD_DIR/stage2.bin"

KERNEL_DIR="kernel"
KERNEL_ELF="$BUILD_DIR/kernel_gde.elf"
KERNEL_BIN="$BUILD_DIR/kernel_gde.bin"

DISK_IMAGE="$BUILD_DIR/pd-os-gde.img"
DISK_SIZE=131072  # 64 MB

CROSS_CC="i686-linux-gnu-gcc"
CROSS_LD="i686-linux-gnu-ld"
CROSS_OBJCOPY="i686-linux-gnu-objcopy"

CFLAGS="-m32 -ffreestanding -nostdlib -nostdinc -fno-builtin \
        -fno-stack-protector -nostartfiles -nodefaultlibs \
        -fno-pic -fno-pie \
        -DGDE_BUILD \
        -Wall -Wextra -c"
IFLAGS="-I$KERNEL_DIR/include"

LDFLAGS="-m elf_i386 -nostdlib"

GREEN='\033[0;32m'
CYAN='\033[0;36m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

# ---- build ----------------------------------------------------------------
build_gde() {
    echo -e "${CYAN}Building PD-OS GDE...${NC}"
    mkdir -p "$BUILD_DIR"

    # --- Stage 1 (identical to CLI) ---
    echo -e "${CYAN}  [1] Assembling Stage 1...${NC}"
    nasm -f bin "$BOOTLOADER_SRC" -o "$BOOTLOADER_BIN"
    echo -e "${GREEN}  [OK] Stage 1${NC}"

    # --- Stage 2 (VBE-enabled) ---
    echo -e "${CYAN}  [2] Assembling Stage 2 (VBE)...${NC}"
    nasm -f bin -DGDE_BUILD "$STAGE2_SRC" -o "$STAGE2_BIN"
    S2SIZE=$(wc -c < "$STAGE2_BIN")
    if [ "$S2SIZE" -gt 3072 ]; then
        echo -e "${RED}[FAIL] Stage 2 is $S2SIZE bytes — exceeds 6-sector limit${NC}"
        exit 1
    fi
    echo -e "${GREEN}  [OK] Stage 2: $S2SIZE bytes${NC}"

    # --- Kernel (GDE build) ---
    echo -e "${CYAN}  [3] Compiling GDE kernel...${NC}"

    # Generate cursor sprite from PNG (requires python3-pil; skipped gracefully if absent)
    python3 "$KERNEL_DIR/PNG/gen_cursor.py" 2>/dev/null || true

    nasm -f elf32 "$KERNEL_DIR/arch/x86/entry.asm"         -o "$BUILD_DIR/entry.o"
    nasm -f elf32 "$KERNEL_DIR/arch/x86/idt.asm"           -o "$BUILD_DIR/idt_stubs.o"
    nasm -f elf32 "$KERNEL_DIR/arch/x86/sched_entry.asm"   -o "$BUILD_DIR/sched_entry.o"

    # Core + drivers
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/drivers/vga.c"          -o "$BUILD_DIR/vga.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/core/io.c"              -o "$BUILD_DIR/io.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/core/panic.c"           -o "$BUILD_DIR/panic.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/arch/x86/idt.c"         -o "$BUILD_DIR/idt.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/arch/x86/pic.c"         -o "$BUILD_DIR/pic.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/arch/x86/exceptions.c"  -o "$BUILD_DIR/exceptions.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/drivers/pit.c"          -o "$BUILD_DIR/pit.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/drivers/keyboard.c"     -o "$BUILD_DIR/keyboard.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/drivers/ata.c"          -o "$BUILD_DIR/ata.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/drivers/pci.c"          -o "$BUILD_DIR/pci.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/drivers/usb.c"          -o "$BUILD_DIR/usb.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/drivers/gfx.c"          -o "$BUILD_DIR/gfx.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/drivers/mouse.c"         -o "$BUILD_DIR/mouse.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/drivers/rtl8139.c"       -o "$BUILD_DIR/rtl8139.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/net/net.c"               -o "$BUILD_DIR/net.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/net/arp.c"               -o "$BUILD_DIR/arp.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/net/ip.c"                -o "$BUILD_DIR/ip.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/net/udp.c"               -o "$BUILD_DIR/udp.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/net/dns.c"               -o "$BUILD_DIR/dns.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/net/tcp.c"               -o "$BUILD_DIR/tcp.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/net/http.c"              -o "$BUILD_DIR/http.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/core/users.c"           -o "$BUILD_DIR/users.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/core/login.c"           -o "$BUILD_DIR/login.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/mm/e820.c"              -o "$BUILD_DIR/e820.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/mm/pmm.c"               -o "$BUILD_DIR/pmm.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/mm/paging.c"            -o "$BUILD_DIR/paging.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/mm/kheap.c"             -o "$BUILD_DIR/kheap.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/fs/vfs.c"               -o "$BUILD_DIR/vfs.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/fs/pdfs.c"              -o "$BUILD_DIR/pdfs.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/fs/fat32.c"             -o "$BUILD_DIR/fat32.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/fs/ext2.c"              -o "$BUILD_DIR/ext2.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/fs/ntfs.c"              -o "$BUILD_DIR/ntfs.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/core/shell.c"           -o "$BUILD_DIR/shell.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/core/install.c"         -o "$BUILD_DIR/install.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/core/process.c"         -o "$BUILD_DIR/process.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/core/de_loader.c"       -o "$BUILD_DIR/de_loader.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/core/kernel.c"          -o "$BUILD_DIR/kernel_main.o"

    # GDE subsystem
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/gde/wm.c"               -o "$BUILD_DIR/wm.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/gde/taskbar.c"          -o "$BUILD_DIR/taskbar.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/gde/terminal.c"         -o "$BUILD_DIR/terminal.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/gde/desktop.c"          -o "$BUILD_DIR/desktop.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/gde/explorer.c"         -o "$BUILD_DIR/explorer.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/gde/text_editor.c"      -o "$BUILD_DIR/text_editor.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/gde/login_screen.c"     -o "$BUILD_DIR/login_screen.o"
    $CROSS_CC $CFLAGS $IFLAGS "$KERNEL_DIR/gde/gde_main.c"         -o "$BUILD_DIR/gde_main.o"

    echo -e "${CYAN}  [4] Linking GDE kernel...${NC}"
    $CROSS_LD $LDFLAGS -T "$KERNEL_DIR/linker.ld" \
        "$BUILD_DIR/entry.o" \
        "$BUILD_DIR/idt_stubs.o" \
        "$BUILD_DIR/sched_entry.o" \
        "$BUILD_DIR/vga.o" \
        "$BUILD_DIR/io.o" \
        "$BUILD_DIR/panic.o" \
        "$BUILD_DIR/idt.o" \
        "$BUILD_DIR/pic.o" \
        "$BUILD_DIR/exceptions.o" \
        "$BUILD_DIR/pit.o" \
        "$BUILD_DIR/keyboard.o" \
        "$BUILD_DIR/ata.o" \
        "$BUILD_DIR/pci.o" \
        "$BUILD_DIR/usb.o" \
        "$BUILD_DIR/gfx.o" \
        "$BUILD_DIR/mouse.o" \
        "$BUILD_DIR/rtl8139.o" \
        "$BUILD_DIR/net.o" \
        "$BUILD_DIR/arp.o" \
        "$BUILD_DIR/ip.o" \
        "$BUILD_DIR/udp.o" \
        "$BUILD_DIR/dns.o" \
        "$BUILD_DIR/tcp.o" \
        "$BUILD_DIR/http.o" \
        "$BUILD_DIR/users.o" \
        "$BUILD_DIR/login.o" \
        "$BUILD_DIR/e820.o" \
        "$BUILD_DIR/pmm.o" \
        "$BUILD_DIR/paging.o" \
        "$BUILD_DIR/kheap.o" \
        "$BUILD_DIR/vfs.o" \
        "$BUILD_DIR/pdfs.o" \
        "$BUILD_DIR/fat32.o" \
        "$BUILD_DIR/ext2.o" \
        "$BUILD_DIR/ntfs.o" \
        "$BUILD_DIR/shell.o" \
        "$BUILD_DIR/install.o" \
        "$BUILD_DIR/process.o" \
        "$BUILD_DIR/de_loader.o" \
        "$BUILD_DIR/kernel_main.o" \
        "$BUILD_DIR/wm.o" \
        "$BUILD_DIR/taskbar.o" \
        "$BUILD_DIR/terminal.o" \
        "$BUILD_DIR/desktop.o" \
        "$BUILD_DIR/explorer.o" \
        "$BUILD_DIR/text_editor.o" \
        "$BUILD_DIR/login_screen.o" \
        "$BUILD_DIR/gde_main.o" \
        -o "$KERNEL_ELF"

    $CROSS_OBJCOPY -O binary "$KERNEL_ELF" "$KERNEL_BIN"
    KSIZE=$(wc -c < "$KERNEL_BIN")
    if [ "$KSIZE" -gt $((320 * 1024)) ]; then
        echo -e "${RED}[FAIL] Kernel $KSIZE bytes exceeds 320 KB stage2 load window${NC}"
        exit 1
    fi
    echo -e "${GREEN}  [OK] GDE Kernel: $KSIZE bytes${NC}"

    # --- Disk image ---
    echo -e "${CYAN}  [5] Creating GDE disk image...${NC}"
    rm -f "$DISK_IMAGE"
    truncate -s $((DISK_SIZE * 512)) "$DISK_IMAGE"
    dd if="$BOOTLOADER_BIN" of="$DISK_IMAGE" conv=notrunc bs=512 seek=0 count=1 2>/dev/null
    dd if="$STAGE2_BIN"     of="$DISK_IMAGE" conv=notrunc bs=512 seek=1           2>/dev/null
    dd if="$KERNEL_BIN"     of="$DISK_IMAGE" conv=notrunc bs=512 seek=7           2>/dev/null
    echo -e "${GREEN}  [OK] GDE disk image: $DISK_IMAGE${NC}"

    # Clear PDFS area
    python3 -c "import sys; sys.stdout.buffer.write(b'\\x00' * (512 * 6))" \
        | dd of="$DISK_IMAGE" conv=notrunc bs=512 seek=1024 2>/dev/null

    echo -e "${GREEN}Build complete: $DISK_IMAGE${NC}"
}

# ---- build + install example DE ------------------------------------------
build_example_de() {
    echo -e "${CYAN}Building example DE...${NC}"
    ./build_de.sh sdk/example_de/example_de.c build/example_de.bin
    echo -e "${CYAN}Installing example DE into disk image...${NC}"
    python3 tools/pdfs_inject.py \
        "$DISK_IMAGE" 1024 \
        "/sys/de/example_de.bin" \
        "build/example_de.bin"
    python3 tools/pdfs_inject.py \
        "$DISK_IMAGE" 1024 \
        "/sys/de/active" \
        <(printf 'example_de')
    echo -e "${GREEN}Example DE installed.  Run ./build_gde.sh run to test.${NC}"
}

# ---- run ------------------------------------------------------------------
run_gde() {
    if [ ! -f "$DISK_IMAGE" ]; then
        build_gde
    fi
    echo -e "${CYAN}Booting GDE in QEMU...${NC}"
    qemu-system-i386 \
        -drive file="$DISK_IMAGE",format=raw,if=ide \
        -m 128M \
        -vga std \
        -display sdl \
        -device rtl8139,netdev=net0 \
        -netdev user,id=net0 \
        2>/dev/null &
}

run_debug() {
    if [ ! -f "$DISK_IMAGE" ]; then
        build_gde
    fi
    echo -e "${CYAN}Booting GDE in QEMU (debug)...${NC}"
    qemu-system-i386 \
        -drive file="$DISK_IMAGE",format=raw,if=ide \
        -m 128M \
        -vga std \
        -display sdl \
        -device rtl8139,netdev=net0 \
        -netdev user,id=net0 \
        -d int,cpu_reset \
        -no-reboot \
        2>&1 | head -200
}

# ---- clean ----------------------------------------------------------------
clean_gde() {
    rm -f "$BUILD_DIR"/*.o "$KERNEL_ELF" "$KERNEL_BIN" "$DISK_IMAGE"
    echo -e "${GREEN}Cleaned GDE build artefacts.${NC}"
}

# ---- dispatch -------------------------------------------------------------
case "$TARGET" in
    all)        build_gde ;;
    run)        run_gde ;;
    run-debug)  run_debug ;;
    build-de)   build_example_de ;;
    clean)      clean_gde ;;
    *)
        echo "Usage: $0 [all|run|run-debug|build-de|clean]"
        exit 1
        ;;
esac
