# PD-OS Development Toolchain Setup Guide

This guide will help you set up the complete development environment for building PD-OS on Windows.

## Required Tools

1. **NASM** - Netwide Assembler (for bootloader and low-level kernel code)
2. **MinGW-w64** - GCC cross-compiler for i686-elf target
3. **QEMU** - x86 system emulator for testing
4. **Make** - Build automation tool
5. **dd** - Disk image creation (via Git Bash or MinGW)

---

## Step 1: Install NASM

NASM is the assembler we'll use for bootloader and CPU-specific assembly code.

### Download & Install:
1. Visit: https://www.nasm.us/pub/nasm/releasebuilds/
2. Download the latest Windows installer (e.g., `nasm-x.xx.xx-installer-x64.exe`)
3. Run the installer
4. **Important**: Check "Add NASM to PATH" during installation

### Verify Installation:
```powershell
nasm -v
```
**Expected output**: `NASM version 2.xx.xx` or similar

---

## Step 2: Install MinGW-w64 (i686-elf Cross-Compiler)

A cross-compiler is needed to build code for 32-bit x86 targets without linking to Windows libraries.

### Option A: Pre-built Binaries (Recommended for Beginners)

1. Visit: https://github.com/lordmilko/i686-elf-tools/releases
2. Download `i686-elf-tools-windows.zip`
3. Extract to `C:\i686-elf-tools\`
4. Add to PATH:
   - Open Start Menu → Search "Environment Variables"
   - Click "Environment Variables" button
   - Under "System variables", select "Path" → Edit
   - Add: `C:\i686-elf-tools\bin`
   - Click OK

### Option B: Build from Source (Advanced)

Follow the guide at: https://wiki.osdev.org/GCC_Cross-Compiler

### Verify Installation:
```powershell
i686-elf-gcc --version
i686-elf-ld --version
```
**Expected output**: `i686-elf-gcc (GCC) x.x.x`

---

## Step 3: Install QEMU

QEMU is an emulator that lets us test PD-OS without rebooting or using real hardware.

### Download & Install:
1. Visit: https://www.qemu.org/download/#windows
2. Download the Windows installer
3. Run the installer (default settings are fine)
4. Add to PATH: `C:\Program Files\qemu\` (or installation directory)

### Verify Installation:
```powershell
qemu-system-i386 --version
```
**Expected output**: `QEMU emulator version x.x.x`

---

## Step 4: Install Make

Make automates the build process.

### Option A: Via Chocolatey (Recommended)

1. Install Chocolatey (if not already installed):
   ```powershell
   Set-ExecutionPolicy Bypass -Scope Process -Force; [System.Net.ServicePointManager]::SecurityProtocol = [System.Net.ServicePointManager]::SecurityProtocol -bor 3072; iex ((New-Object System.Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))
   ```

2. Install Make:
   ```powershell
   choco install make
   ```

### Option B: Via MinGW-w64 (Alternative)

1. Download: https://sourceforge.net/projects/mingw-w64/
2. Install with MSYS2
3. Run: `pacman -S make`

### Option C: Use Git Bash (If Git is installed)

Git for Windows includes `make` in Git Bash.

### Verify Installation:
```powershell
make --version
```
**Expected output**: `GNU Make x.x`

---

## Step 5: Install Git Bash (Optional but Recommended)

Git Bash provides Unix-like tools including `dd` for disk image creation.

### Download & Install:
1. Visit: https://git-scm.com/download/win
2. Download and run the installer
3. Default options work fine

### Verify Installation:
```bash
# In Git Bash:
dd --version
```

---

## Step 6: Verify Complete Setup

Run this verification script to ensure everything is installed correctly:

```powershell
# Create verification script
Write-Output "=== PD-OS Toolchain Verification ==="
Write-Output ""

Write-Output "Checking NASM..."
nasm -v
if ($?) { Write-Output "✓ NASM installed" } else { Write-Output "✗ NASM missing" }
Write-Output ""

Write-Output "Checking i686-elf-gcc..."
i686-elf-gcc --version | Select-Object -First 1
if ($?) { Write-Output "✓ Cross-compiler installed" } else { Write-Output "✗ Cross-compiler missing" }
Write-Output ""

Write-Output "Checking QEMU..."
qemu-system-i386 --version | Select-Object -First 1
if ($?) { Write-Output "✓ QEMU installed" } else { Write-Output "✗ QEMU missing" }
Write-Output ""

Write-Output "Checking Make..."
make --version | Select-Object -First 1
if ($?) { Write-Output "✓ Make installed" } else { Write-Output "✗ Make missing" }
Write-Output ""

Write-Output "=== Verification Complete ==="
```

---

## Troubleshooting

### "Command not found" errors
- Ensure all tools are added to your system PATH
- Restart PowerShell/Terminal after modifying PATH
- Verify PATH: `$env:Path -split ';'`

### QEMU won't start
- Check Windows Firewall settings
- Run PowerShell as Administrator

### Cross-compiler linking errors
- Ensure you're using `i686-elf-gcc`, not regular `gcc`
- Use `-nostdlib -nostartfiles` flags
- Don't link against Windows libraries

### Make errors
- On Windows, use PowerShell or Git Bash
- Check Makefile uses tabs (not spaces) for indentation
- Use Unix-style paths in Makefile

---

## Alternative: WSL (Windows Subsystem for Linux)

If you encounter persistent issues with Windows tools, consider using WSL:

```powershell
# Enable WSL
wsl --install -d Ubuntu

# Inside WSL:
sudo apt update
sudo apt install nasm build-essential qemu-system-x86
```

However, this guide assumes native Windows tools as requested.

---

## Next Steps

Once all tools are installed and verified:
1. Navigate to project root: `cd Custom-Bootloader-Kernel-CLI-GUI`
2. Test build system: `make help` (after Makefile is created)
3. Start Phase 2: Building PD-Bootloader

---

## Quick Reference

| Tool | Command | Purpose |
|------|---------|---------|
| NASM | `nasm -f bin file.asm -o file.bin` | Assemble bootloader |
| GCC | `i686-elf-gcc -c file.c -o file.o` | Compile kernel C code |
| LD | `i686-elf-ld -T linker.ld -o kernel.elf` | Link kernel |
| QEMU | `qemu-system-i386 -drive format=raw,file=os.img` | Test OS |
| Make | `make all` | Build entire project |
| dd | `dd if=boot.bin of=os.img` | Create disk image |

---

## Additional Resources

- NASM Manual: https://www.nasm.us/doc/
- OSDev Wiki: https://wiki.osdev.org/
- GCC Cross-Compiler: https://wiki.osdev.org/GCC_Cross-Compiler
- QEMU Documentation: https://www.qemu.org/docs/master/

---

**Status**: ✓ Setup guide complete  
**Next**: Create root Makefile and begin bootloader development
