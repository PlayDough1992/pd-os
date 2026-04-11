# Manual Toolchain Installation Guide

## NASM Installation

NASM installer has been downloaded to: `tools-download\nasm-installer.exe`

**To install:**
1. Run the installer: `.\tools-download\nasm-installer.exe`
2. During installation, **CHECK the box** that says "Add NASM to PATH"
3. Complete the installation
4. Restart PowerShell

**Verify:**
```powershell
nasm -v
```
Should show: `NASM version 2.16.03` or similar

---

## i686-elf-gcc Cross-Compiler Installation

The automatic download failed. Here's how to install manually:

### Option 1: Download Pre-built Binary (Recommended)

1. **Download:**
   - Go to: https://github.com/lordmilko/i686-elf-tools/releases
   - Download: `i686-elf-tools-windows-13.2.0.zip` (about 270 MB)
   
2. **Extract:**
   - Create folder: `C:\i686-elf-tools`
   - Extract the ZIP contents there
   - You should have: `C:\i686-elf-tools\bin\i686-elf-gcc.exe`

3. **Add to PATH:**
   ```powershell
   # Run this in PowerShell (as Administrator):
   $path = [Environment]::GetEnvironmentVariable("Path", "User")
   [Environment]::SetEnvironmentVariable("Path", "$path;C:\i686-elf-tools\bin", "User")
   ```
   OR manually:
   - Open Start → Search "Environment Variables"
   - Edit "Path" variable
   - Add: `C:\i686-elf-tools\bin`

4. **Verify (after restarting PowerShell):**
   ```powershell
   i686-elf-gcc --version
   ```

### Option 2: Use Pre-installed GCC (Quick Test Only)

For quick testing, you can try using regular MinGW GCC if installed, but this is not recommended for production:
```powershell
# Check if MinGW is installed
gcc --version
```

---

## Quick Status Check

After installing, run this to verify everything:
```powershell
.\build.ps1 check
```

---

## What's Required for Each Phase

| Phase | NASM | i686-elf-gcc | QEMU |
|-------|------|--------------|------|
| 1-3: Bootloader | ✓ Required | - | ✓ Testing |
| 4+: Kernel | - | ✓ Required | ✓ Testing |

**For now (Phase 2), you only need:**
- ✓ NASM (to build bootloader)
- ✓ QEMU (already installed, to test)

**The cross-compiler is only needed starting in Phase 4 (kernel development).**

---

## Alternative: Start Building Now!

Since you have NASM downloaded and QEMU installed, you can:

1. **Restart PowerShell** (to refresh PATH after NASM install)

2. **Build the bootloader:**
   ```powershell
   .\build.ps1 all
   ```

3. **Run in QEMU:**
   ```powershell
   .\build.ps1 run
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
