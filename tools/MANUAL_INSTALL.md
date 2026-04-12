# Manual Toolchain Installation Guide (Linux)

## NASM Installation

Install NASM using your distro's package manager:

**Ubuntu / Debian:**
```bash
sudo apt-get update && sudo apt-get install nasm
```

**Fedora / RHEL:**
```bash
sudo dnf install nasm
```

**Arch Linux:**
```bash
sudo pacman -S nasm
```

**Verify:**
```bash
nasm -v
```
Should show: `NASM version 2.xx.xx`

---

## i686-elf-gcc Cross-Compiler Installation

> Only required for **Phase 4+** (kernel development). Skip for now if you are in Phase 1–3.

### Option 1: Package Manager (Quick, but may use linux-gnu instead of elf)

**Ubuntu / Debian:**
```bash
sudo apt-get install gcc-i686-linux-gnu binutils-i686-linux-gnu
```
Use `i686-linux-gnu-gcc` in the Makefile. For a true freestanding `i686-elf-gcc`, use Option 2.

**Arch Linux:**
```bash
sudo pacman -S cross-i686-elf-gcc cross-i686-elf-binutils
```

### Option 2: Build from Source (Recommended for OS Development)

This produces a true freestanding `i686-elf-gcc` with no host-OS dependencies.

**Prerequisites (Ubuntu / Debian):**
```bash
sudo apt-get install build-essential bison flex libgmp-dev libmpc-dev libmpfr-dev texinfo
```

**Set up environment:**
```bash
export PREFIX="$HOME/opt/cross"
export TARGET=i686-elf
export PATH="$PREFIX/bin:$PATH"
mkdir -p "$PREFIX"
```

**Build Binutils:**
```bash
# Download from https://ftp.gnu.org/gnu/binutils/
tar -xf binutils-2.42.tar.gz
mkdir build-binutils && cd build-binutils
../binutils-2.42/configure --target=$TARGET --prefix=$PREFIX --with-sysroot --disable-nls --disable-werror
make && make install
cd ..
```

**Build GCC (cross-compiler only):**
```bash
# Download from https://ftp.gnu.org/gnu/gcc/gcc-13.2.0/
tar -xf gcc-13.2.0.tar.gz
mkdir build-gcc && cd build-gcc
../gcc-13.2.0/configure --target=$TARGET --prefix=$PREFIX --disable-nls --enable-languages=c,c++ --without-headers
make all-gcc && make all-target-libgcc
make install-gcc && make install-target-libgcc
cd ..
```

**Add to PATH permanently:**
```bash
echo 'export PATH="$HOME/opt/cross/bin:$PATH"' >> ~/.bashrc
source ~/.bashrc
```

**Verify:**
```bash
i686-elf-gcc --version
i686-elf-ld --version
```

Full guide: https://wiki.osdev.org/GCC_Cross-Compiler

---

## QEMU Installation

**Ubuntu / Debian:**
```bash
sudo apt-get install qemu-system-x86
```

**Fedora / RHEL:**
```bash
sudo dnf install qemu-system-x86
```

**Arch Linux:**
```bash
sudo pacman -S qemu-arch-extra
```

**Verify:**
```bash
qemu-system-i386 --version
```

---

## Quick Status Check

After installing, verify everything:
```bash
./build.sh setup-check
# or
make setup-check
```

---

## What's Required for Each Phase

| Phase | NASM | i686-elf-gcc | QEMU |
|-------|------|--------------|------|
| 1–3: Bootloader | ✓ Required | — | ✓ Testing |
| 4+: Kernel | — | ✓ Required | ✓ Testing |

**For Phase 1–3 (current), you only need:**
- NASM (assembler)
- QEMU (emulator)
- Make

**The cross-compiler is only needed starting in Phase 4 (kernel development).**

---

## Build Now!

Once NASM and QEMU are installed:

```bash
# Build the bootloader
./build.sh all
# or: make all

# Run in QEMU
./build.sh run
# or: make run
```

The cross-compiler can be installed later before Phase 4.

---

## Troubleshooting

### "NASM not found" after installation
- Did you check "Add to PATH" during installation?
- Did you restart PowerShell?
- Try running from a new PowerShell window

### Can't add to PATH
Run PowerShell as Administrator:
```powershell
Start-Process powershell -Verb RunAs
```

### Still having issues?
You can build manually without scripts:
```powershell
# Navigate to project
cd E:\Projects_other\Custom-Bootloader-Kernel-CLI-GUI

# Build bootloader (once NASM is in PATH)
nasm -f bin bootloader\stage1.asm -o build\bootloader.bin

# Check size (must be 512 bytes)
(Get-Item build\bootloader.bin).Length

# Run in QEMU
qemu-system-i386 -drive format=raw,file=build\pd-os.img
```
