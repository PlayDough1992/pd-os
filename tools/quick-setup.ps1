# Quick Setup Script for PD-OS Tools (Without Chocolatey)
# Run this script to download and help install required tools

Write-Host "=== PD-OS Quick Setup (No Chocolatey Required) ===" -ForegroundColor Cyan
Write-Host ""

# Create downloads directory
$downloadDir = ".\tools-download"
if (-not (Test-Path $downloadDir)) {
    New-Item -ItemType Directory -Path $downloadDir | Out-Null
    Write-Host "Created download directory: $downloadDir" -ForegroundColor Green
}

Write-Host ""
Write-Host "=== Step 1: NASM (Assembler) ===" -ForegroundColor Yellow
Write-Host "Downloading NASM installer..."

try {
    $nasmUrl = "https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/win64/nasm-2.16.03-installer-x64.exe"
    $nasmInstaller = "$downloadDir\nasm-installer.exe"
    
    Invoke-WebRequest -Uri $nasmUrl -OutFile $nasmInstaller -UseBasicParsing
    Write-Host "[OK] NASM installer downloaded to: $nasmInstaller" -ForegroundColor Green
    Write-Host ""
    Write-Host "To install NASM:" -ForegroundColor White
    Write-Host "  1. Run: .\$nasmInstaller" -ForegroundColor Cyan
    Write-Host "  2. Follow the installer wizard" -ForegroundColor Cyan
    Write-Host "  3. IMPORTANT: Check 'Add NASM to PATH' option" -ForegroundColor Yellow
    Write-Host ""
} catch {
    Write-Host "[ERROR] Download failed. Please download manually from:" -ForegroundColor Red
    Write-Host "  https://www.nasm.us/pub/nasm/releasebuilds/" -ForegroundColor White
}

Write-Host ""
Write-Host "=== Step 2: i686-elf-gcc (Cross-Compiler) ===" -ForegroundColor Yellow
Write-Host "Downloading i686-elf-tools..."

try {
    $gccUrl = "https://github.com/lordmilko/i686-elf-tools/releases/download/13.2.0/i686-elf-tools-windows-13.2.0.zip"
    $gccZip = "$downloadDir\i686-elf-tools.zip"
    $gccExtractPath = "C:\i686-elf-tools"
    
    Write-Host "Downloading cross-compiler (this may take a few minutes)..." -ForegroundColor Cyan
    Invoke-WebRequest -Uri $gccUrl -OutFile $gccZip -UseBasicParsing
    Write-Host "[OK] Cross-compiler downloaded" -ForegroundColor Green
    
    Write-Host "Extracting to $gccExtractPath..." -ForegroundColor Cyan
    Expand-Archive -Path $gccZip -DestinationPath $gccExtractPath -Force
    Write-Host "[OK] Cross-compiler extracted" -ForegroundColor Green
    
    # Add to PATH
    $binPath = "$gccExtractPath\bin"
    $currentPath = [Environment]::GetEnvironmentVariable("Path", "User")
    if ($currentPath -notlike "*$binPath*") {
        Write-Host "Adding to PATH..." -ForegroundColor Cyan
        $newPath = "$currentPath;$binPath"
        [Environment]::SetEnvironmentVariable("Path", $newPath, "User")
        Write-Host "[OK] Added to PATH (restart terminal to use)" -ForegroundColor Green
    }
} catch {
    Write-Host "[ERROR] Download failed. Please download manually from:" -ForegroundColor Red
    Write-Host "  https://github.com/lordmilko/i686-elf-tools/releases" -ForegroundColor White
    Write-Host "  Extract to: C:\i686-elf-tools" -ForegroundColor White
    Write-Host "  Add to PATH: C:\i686-elf-tools\bin" -ForegroundColor White
}

Write-Host ""
Write-Host "=== Step 3: Make (Build Automation) ===" -ForegroundColor Yellow
Write-Host ""
Write-Host "Option A: Use Git Bash (Recommended if Git is installed)" -ForegroundColor Cyan
$gitBashPath = "C:\Program Files\Git\bin\bash.exe"
if (Test-Path $gitBashPath) {
    Write-Host "[OK] Git Bash is already installed at: C:\Program Files\Git" -ForegroundColor Green
    Write-Host "  You can use 'make' from Git Bash" -ForegroundColor White
} else {
    Write-Host "Git not found. You can:" -ForegroundColor Yellow
    Write-Host "  1. Install Git for Windows from: https://git-scm.com/download/win" -ForegroundColor White
    Write-Host "  2. Or use PowerShell build scripts (see below)" -ForegroundColor White
}

Write-Host ""
Write-Host "Option B: Build without Make (use PowerShell scripts)" -ForegroundColor Cyan
Write-Host "  Use build.ps1 instead of Make" -ForegroundColor White

Write-Host ""
Write-Host "=== Setup Complete! ===" -ForegroundColor Green
Write-Host ""
Write-Host "Next steps:" -ForegroundColor Yellow
Write-Host "  1. Run NASM installer: .\tools-download\nasm-installer.exe" -ForegroundColor White
Write-Host "  2. Close and reopen PowerShell (to refresh PATH)" -ForegroundColor White
Write-Host "  3. Build bootloader: .\build.ps1 all" -ForegroundColor White
Write-Host "  4. Run in QEMU: .\build.ps1 run" -ForegroundColor White
Write-Host ""
