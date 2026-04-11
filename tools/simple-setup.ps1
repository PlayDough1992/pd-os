# =============================================================================
# PD-OS SUPER SIMPLE SETUP - Step by Step
# =============================================================================
# This script will guide you through each step with clear instructions
# =============================================================================

Write-Host ""
Write-Host "===============================================" -ForegroundColor Cyan
Write-Host "  PD-OS SETUP - Step by Step Guide" -ForegroundColor Cyan
Write-Host "===============================================" -ForegroundColor Cyan
Write-Host ""

# Step 1: Install NASM
Write-Host "STEP 1: Install NASM Assembler" -ForegroundColor Yellow
Write-Host "-------------------------------" -ForegroundColor Yellow

if (Get-Command nasm -ErrorAction SilentlyContinue) {
    Write-Host "[OK] NASM is already installed!" -ForegroundColor Green
    & nasm -v
} else {
    Write-Host "[INFO] NASM is not installed yet" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "The NASM installer is ready at:" -ForegroundColor White
    Write-Host "  .\tools-download\nasm-installer.exe" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "I will now open the installer for you." -ForegroundColor White
    Write-Host "IMPORTANT: During installation, CHECK the box that says:" -ForegroundColor Yellow
    Write-Host "  'Add NASM to the system PATH'" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "Press any key to open the NASM installer..." -ForegroundColor Green
    $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
    
    Start-Process -FilePath ".\tools-download\nasm-installer.exe" -Wait
    
    Write-Host ""
    Write-Host "[INFO] Installer finished. Checking if NASM is now available..." -ForegroundColor Cyan
    
    # Refresh environment
    $env:Path = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path","User")
    
    if (Get-Command nasm -ErrorAction SilentlyContinue) {
        Write-Host "[OK] NASM installed successfully!" -ForegroundColor Green
    } else {
        Write-Host "[WARNING] NASM not found in PATH" -ForegroundColor Red
        Write-Host ""
        Write-Host "Please close this PowerShell window and open a NEW one" -ForegroundColor Yellow
        Write-Host "Then run this script again: .\tools\simple-setup.ps1" -ForegroundColor Yellow
        Write-Host ""
        exit 1
    }
}

Write-Host ""
Write-Host ""

# Step 2: Check QEMU
Write-Host "STEP 2: Check QEMU Emulator" -ForegroundColor Yellow
Write-Host "-------------------------------" -ForegroundColor Yellow

if (Get-Command qemu-system-i386 -ErrorAction SilentlyContinue) {
    Write-Host "[OK] QEMU is installed!" -ForegroundColor Green
    & qemu-system-i386 --version | Select-Object -First 1
} else {
    Write-Host "[WARNING] QEMU is not installed" -ForegroundColor Red
    Write-Host "Download from: https://www.qemu.org/download/#windows" -ForegroundColor Yellow
    Write-Host "QEMU is needed to test your OS" -ForegroundColor Yellow
}

Write-Host ""
Write-Host ""

# Step 3: Build Bootloader
Write-Host "STEP 3: Build the Bootloader" -ForegroundColor Yellow
Write-Host "-------------------------------" -ForegroundColor Yellow

if (-not (Get-Command nasm -ErrorAction SilentlyContinue)) {
    Write-Host "[SKIP] NASM not available, cannot build" -ForegroundColor Red
} else {
    Write-Host "Building PD-Bootloader..." -ForegroundColor Cyan
    
    # Create build directory
    if (-not (Test-Path build)) {
        New-Item -ItemType Directory -Path build | Out-Null
    }
    
    # Build bootloader
    & nasm -f bin bootloader\stage1.asm -o build\bootloader.bin
    
    if ($LASTEXITCODE -eq 0) {
        $size = (Get-Item build\bootloader.bin).Length
        if ($size -eq 512) {
            Write-Host "[OK] Bootloader built successfully! (512 bytes)" -ForegroundColor Green
            
            # Create disk image
            Write-Host "Creating disk image..." -ForegroundColor Cyan
            
            $diskImg = "build\pd-os.img"
            $blank = New-Object byte[] (1440 * 1024)
            [IO.File]::WriteAllBytes((Join-Path (Get-Location) $diskImg), $blank)
            
            $boot = [IO.File]::ReadAllBytes((Join-Path (Get-Location) "build\bootloader.bin"))
            $disk = [IO.File]::ReadAllBytes((Join-Path (Get-Location) $diskImg))
            
            for ($i = 0; $i -lt 512; $i++) {
                $disk[$i] = $boot[$i]
            }
            
            [IO.File]::WriteAllBytes((Join-Path (Get-Location) $diskImg), $disk)
            Write-Host "[OK] Disk image created!" -ForegroundColor Green
            
        } else {
            Write-Host "[ERROR] Bootloader is $size bytes, should be 512" -ForegroundColor Red
        }
    } else {
        Write-Host "[ERROR] Build failed" -ForegroundColor Red
    }
}

Write-Host ""
Write-Host ""

# Step 4: Run in QEMU
Write-Host "STEP 4: Test in QEMU" -ForegroundColor Yellow
Write-Host "-------------------------------" -ForegroundColor Yellow

if ((Test-Path "build\pd-os.img") -and (Get-Command qemu-system-i386 -ErrorAction SilentlyContinue)) {
    Write-Host "Ready to test PD-OS!" -ForegroundColor Green
    Write-Host ""
    Write-Host "When QEMU starts, you should see:" -ForegroundColor White
    Write-Host "  PD-Bootloader v0.1 - Stage 1" -ForegroundColor Cyan
    Write-Host "  Booting PD-OS..." -ForegroundColor Cyan
    Write-Host "  Loading Stage 2..." -ForegroundColor Cyan
    Write-Host ""
    Write-Host "Press any key to start QEMU..." -ForegroundColor Green
    $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
    
    Write-Host ""
    Write-Host "[INFO] Starting QEMU (Press Ctrl+Alt+G to release mouse)" -ForegroundColor Cyan
    & qemu-system-i386 -drive format=raw,file=build\pd-os.img -m 128M
    
    Write-Host ""
    Write-Host "[OK] QEMU closed" -ForegroundColor Green
} else {
    if (-not (Test-Path "build\pd-os.img")) {
        Write-Host "[SKIP] Disk image not built yet" -ForegroundColor Yellow
    }
    if (-not (Get-Command qemu-system-i386 -ErrorAction SilentlyContinue)) {
        Write-Host "[SKIP] QEMU not installed" -ForegroundColor Yellow
    }
}

Write-Host ""
Write-Host "===============================================" -ForegroundColor Cyan
Write-Host "  Setup Complete!" -ForegroundColor Cyan
Write-Host "===============================================" -ForegroundColor Cyan
Write-Host ""

if ((Test-Path "build\pd-os.img") -and (Get-Command qemu-system-i386 -ErrorAction SilentlyContinue)) {
    Write-Host "[SUCCESS] PD-OS is ready to run!" -ForegroundColor Green
    Write-Host ""
    Write-Host "Next time, just run:" -ForegroundColor White
    Write-Host "  .\build.ps1 run" -ForegroundColor Cyan
} else {
    Write-Host "Next steps:" -ForegroundColor Yellow
    Write-Host "  1. Close this PowerShell window" -ForegroundColor White
    Write-Host "  2. Open a NEW PowerShell window" -ForegroundColor White
    Write-Host "  3. Run: .\tools\simple-setup.ps1" -ForegroundColor White
}

Write-Host ""
