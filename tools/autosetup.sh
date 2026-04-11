#!/bin/bash
# ============================================================================
# PD-OS FULLY AUTOMATED SETUP (Linux)
# ============================================================================
# Detects your distro and installs all required tools automatically.
# Usage: ./tools/autosetup.sh
# ============================================================================

set -e

GREEN='\033[0;32m'
CYAN='\033[0;36m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo ""
echo -e "${CYAN}============================================================${NC}"
echo -e "${CYAN}  PD-OS FULLY AUTOMATED SETUP (Linux)${NC}"
echo -e "${CYAN}============================================================${NC}"
echo ""

TOTAL_STEPS=4
STEP=0

step() {
    STEP=$((STEP + 1))
    echo -e "${YELLOW}[$STEP/$TOTAL_STEPS] $1${NC}"
}

ok()   { echo -e "  ${GREEN}[OK]${NC} $1"; }
skip() { echo -e "  [SKIP] $1"; }
fail() { echo -e "  ${RED}[ERROR]${NC} $1"; }

# ============================================================================
# Detect package manager
# ============================================================================
if command -v apt-get > /dev/null 2>&1; then
    PKG_MANAGER="apt"
elif command -v dnf > /dev/null 2>&1; then
    PKG_MANAGER="dnf"
elif command -v pacman > /dev/null 2>&1; then
    PKG_MANAGER="pacman"
else
    echo -e "${RED}Could not detect a supported package manager (apt/dnf/pacman).${NC}"
    echo "Please install NASM, i686-elf-gcc, QEMU, and make manually."
    echo "See tools/setup.md for details."
    exit 1
fi

ok "Detected package manager: $PKG_MANAGER"
echo ""

install_pkg() {
    case "$PKG_MANAGER" in
        apt)    sudo apt-get install -y "$@" ;;
        dnf)    sudo dnf install -y "$@" ;;
        pacman) sudo pacman -S --noconfirm "$@" ;;
    esac
}

# ============================================================================
# Step 1: NASM
# ============================================================================
step "Installing NASM Assembler"
if command -v nasm > /dev/null 2>&1; then
    skip "NASM already installed: $(nasm -v | head -1)"
else
    case "$PKG_MANAGER" in
        apt)    install_pkg nasm ;;
        dnf)    install_pkg nasm ;;
        pacman) install_pkg nasm ;;
    esac
    ok "NASM installed: $(nasm -v | head -1)"
fi
echo ""

# ============================================================================
# Step 2: QEMU
# ============================================================================
step "Installing QEMU"
if command -v qemu-system-i386 > /dev/null 2>&1; then
    skip "QEMU already installed: $(qemu-system-i386 --version | head -1)"
else
    case "$PKG_MANAGER" in
        apt)    install_pkg qemu-system-x86 ;;
        dnf)    install_pkg qemu-system-x86 ;;
        pacman) install_pkg qemu-arch-extra ;;
    esac
    ok "QEMU installed: $(qemu-system-i386 --version | head -1)"
fi
echo ""

# ============================================================================
# Step 3: Make
# ============================================================================
step "Installing Make"
if command -v make > /dev/null 2>&1; then
    skip "Make already installed: $(make --version | head -1)"
else
    case "$PKG_MANAGER" in
        apt)    install_pkg make ;;
        dnf)    install_pkg make ;;
        pacman) install_pkg make ;;
    esac
    ok "Make installed"
fi
echo ""

# ============================================================================
# Step 4: i686-elf cross-compiler (needed for Phase 4+ kernel development)
# ============================================================================
step "Installing i686-elf cross-compiler (needed for Phase 4+)"
if command -v i686-elf-gcc > /dev/null 2>&1; then
    skip "i686-elf-gcc already installed"
else
    case "$PKG_MANAGER" in
        apt)
            # Ubuntu/Debian: build from source or use cross-gcc package
            if apt-cache show gcc-i686-linux-gnu > /dev/null 2>&1; then
                install_pkg gcc-i686-linux-gnu binutils-i686-linux-gnu
                ok "Installed gcc-i686-linux-gnu (note: use i686-linux-gnu-gcc)"
            else
                echo -e "  ${YELLOW}[INFO]${NC} i686-elf-gcc not in apt repos."
                echo "  See tools/MANUAL_INSTALL.md to build from source."
                echo "  (Only needed for Phase 4+ kernel development)"
            fi
            ;;
        dnf)
            echo -e "  ${YELLOW}[INFO]${NC} i686-elf-gcc not in default dnf repos."
            echo "  See tools/MANUAL_INSTALL.md to build from source."
            echo "  (Only needed for Phase 4+ kernel development)"
            ;;
        pacman)
            if pacman -Si cross-i686-elf-gcc > /dev/null 2>&1; then
                install_pkg cross-i686-elf-gcc cross-i686-elf-binutils
                ok "i686-elf-gcc installed via AUR/community"
            else
                echo -e "  ${YELLOW}[INFO]${NC} See tools/MANUAL_INSTALL.md"
                echo "  (Only needed for Phase 4+ kernel development)"
            fi
            ;;
    esac
fi
echo ""

# ============================================================================
# Summary
# ============================================================================
echo -e "${CYAN}============================================================${NC}"
echo -e "${CYAN}  Setup Complete!${NC}"
echo -e "${CYAN}============================================================${NC}"
echo ""
echo -e "${YELLOW}Toolchain status:${NC}"
for tool in nasm i686-elf-gcc qemu-system-i386 make; do
    printf "  %-25s" "$tool"
    if command -v "$tool" > /dev/null 2>&1; then
        echo -e "${GREEN}OK${NC}"
    else
        echo -e "${RED}NOT FOUND${NC}"
    fi
done
echo ""
echo -e "${YELLOW}Next steps:${NC}"
echo "  1. Build the bootloader:  make all"
echo "  2. Run in QEMU:           make run"
echo "  (Or use ./build.sh all / ./build.sh run)"
echo ""
