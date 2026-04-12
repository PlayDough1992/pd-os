#!/bin/bash
# ============================================================================
# PD-OS SIMPLE SETUP - Step by Step (Linux)
# ============================================================================
# An interactive guide to set up your Linux development environment.
# Usage: ./tools/simple-setup.sh
# ============================================================================

GREEN='\033[0;32m'
CYAN='\033[0;36m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo ""
echo -e "${CYAN}===============================================${NC}"
echo -e "${CYAN}  PD-OS SETUP - Step by Step Guide (Linux)${NC}"
echo -e "${CYAN}===============================================${NC}"
echo ""

pause() {
    echo -e "${GREEN}Press Enter to continue...${NC}"
    read -r
}

# --------------------------------------------------------------------------
# Step 1: NASM
# --------------------------------------------------------------------------
echo -e "${YELLOW}STEP 1: Install NASM Assembler${NC}"
echo "-------------------------------"

if command -v nasm > /dev/null 2>&1; then
    echo -e "${GREEN}[OK] NASM is already installed!${NC}"
    nasm -v
else
    echo -e "${CYAN}[INFO] NASM is not installed.${NC}"
    echo ""
    echo "Install it with your package manager:"
    echo -e "  ${CYAN}Ubuntu/Debian:${NC}  sudo apt-get install nasm"
    echo -e "  ${CYAN}Fedora/RHEL:${NC}   sudo dnf install nasm"
    echo -e "  ${CYAN}Arch Linux:${NC}    sudo pacman -S nasm"
    echo ""
    pause
    echo -e "${CYAN}Checking again...${NC}"
    if command -v nasm > /dev/null 2>&1; then
        echo -e "${GREEN}[OK] NASM is now installed!${NC}"
    else
        echo -e "${RED}[WARNING] NASM still not found. Please install it before continuing.${NC}"
    fi
fi

echo ""
echo ""

# --------------------------------------------------------------------------
# Step 2: QEMU
# --------------------------------------------------------------------------
echo -e "${YELLOW}STEP 2: Check QEMU Emulator${NC}"
echo "-------------------------------"

if command -v qemu-system-i386 > /dev/null 2>&1; then
    echo -e "${GREEN}[OK] QEMU is installed!${NC}"
    qemu-system-i386 --version | head -1
else
    echo -e "${RED}[WARNING] QEMU is not installed.${NC}"
    echo ""
    echo "Install it with your package manager:"
    echo -e "  ${CYAN}Ubuntu/Debian:${NC}  sudo apt-get install qemu-system-x86"
    echo -e "  ${CYAN}Fedora/RHEL:${NC}   sudo dnf install qemu-system-x86"
    echo -e "  ${CYAN}Arch Linux:${NC}    sudo pacman -S qemu-arch-extra"
    echo ""
    pause
fi

echo ""
echo ""

# --------------------------------------------------------------------------
# Step 3: Build Bootloader
# --------------------------------------------------------------------------
echo -e "${YELLOW}STEP 3: Build the Bootloader${NC}"
echo "-------------------------------"

if ! command -v nasm > /dev/null 2>&1; then
    echo -e "${RED}[SKIP] NASM not found, cannot build.${NC}"
else
    echo -e "${CYAN}Building PD-Bootloader...${NC}"
    mkdir -p build
    nasm -f bin bootloader/stage1.asm -o build/bootloader.bin
    SIZE=$(wc -c < build/bootloader.bin)
    if [ "$SIZE" -eq 512 ]; then
        echo -e "${GREEN}[OK] Bootloader built successfully! (512 bytes)${NC}"

        echo -e "${CYAN}Creating disk image...${NC}"
        dd if=/dev/zero of=build/pd-os.img bs=512 count=2880 2>/dev/null
        dd if=build/bootloader.bin of=build/pd-os.img conv=notrunc bs=512 count=1 2>/dev/null
        echo -e "${GREEN}[OK] Disk image created: build/pd-os.img${NC}"
    else
        echo -e "${RED}[FAIL] Bootloader is $SIZE bytes (expected 512)${NC}"
    fi
fi

echo ""
echo ""

# --------------------------------------------------------------------------
# Step 4: Run in QEMU
# --------------------------------------------------------------------------
echo -e "${YELLOW}STEP 4: Run in QEMU${NC}"
echo "-------------------------------"

if [ ! -f build/pd-os.img ]; then
    echo -e "${RED}[SKIP] Disk image not found. Complete Step 3 first.${NC}"
elif ! command -v qemu-system-i386 > /dev/null 2>&1; then
    echo -e "${RED}[SKIP] QEMU not installed. Complete Step 2 first.${NC}"
else
    echo -e "${CYAN}Starting QEMU...${NC}"
    echo "Press Ctrl+Alt+G to release mouse, Ctrl+C to quit"
    qemu-system-i386 -drive format=raw,file=build/pd-os.img -m 128M
fi

echo ""
echo -e "${GREEN}Setup guide complete!${NC}"
