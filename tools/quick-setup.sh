#!/bin/bash
# ============================================================================
# PD-OS Quick Setup Helper (Linux)
# ============================================================================
# Prints install commands for all required tools and verifies what is present.
# Usage: ./tools/quick-setup.sh
# ============================================================================

GREEN='\033[0;32m'
CYAN='\033[0;36m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${CYAN}=== PD-OS Quick Setup (Linux) ===${NC}"
echo ""

check_tool() {
    local name="$1"
    printf "  %-25s" "$name"
    if command -v "$name" > /dev/null 2>&1; then
        echo -e "${GREEN}Found${NC}"
    else
        echo -e "${RED}Not found${NC}"
    fi
}

echo -e "${YELLOW}=== Current Toolchain Status ===${NC}"
check_tool nasm
check_tool i686-elf-gcc
check_tool qemu-system-i386
check_tool make
check_tool dd
echo ""

echo -e "${YELLOW}=== Install Commands by Distro ===${NC}"
echo ""

echo -e "${CYAN}Ubuntu / Debian:${NC}"
echo "  sudo apt-get update"
echo "  sudo apt-get install -y nasm qemu-system-x86 make"
echo "  # For cross-compiler (Phase 4+):"
echo "  sudo apt-get install -y gcc-i686-linux-gnu binutils-i686-linux-gnu"
echo ""

echo -e "${CYAN}Fedora / RHEL / CentOS:${NC}"
echo "  sudo dnf install -y nasm qemu-system-x86 make"
echo "  # For cross-compiler (Phase 4+), build from source - see MANUAL_INSTALL.md"
echo ""

echo -e "${CYAN}Arch Linux / Manjaro:${NC}"
echo "  sudo pacman -S nasm qemu-arch-extra make"
echo "  # For cross-compiler (Phase 4+):"
echo "  sudo pacman -S cross-i686-elf-gcc cross-i686-elf-binutils"
echo ""

echo -e "${YELLOW}=== What's Needed per Phase ===${NC}"
echo "  Phase 1-3 (Bootloader):  nasm + qemu-system-i386 + make"
echo "  Phase 4+  (Kernel):      + i686-elf-gcc + i686-elf-ld"
echo ""

echo -e "${YELLOW}=== Build Commands ===${NC}"
echo "  ./build.sh all     - Build bootloader + disk image"
echo "  ./build.sh run     - Build and launch in QEMU"
echo "  make all           - Same as above via Makefile"
echo "  make run           - Build and run via Makefile"
echo ""

echo -e "${YELLOW}=== For automated install run: ===${NC}"
echo "  ./tools/autosetup.sh"
echo ""
