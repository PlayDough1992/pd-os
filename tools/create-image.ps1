# ============================================================================
# PD-OS Disk Image Builder
# ============================================================================
# This script creates a bootable disk image for PD-OS
# Usage: .\tools\create-image.ps1
# ============================================================================

param(
    [string]$BootloaderPath = "build\bootloader.bin",
    [string]$OutputImage = "build\pd-os.img",
    [int]$DiskSizeKB = 1440  # 1.44MB floppy disk
)

Write-Host "=== PD-OS Disk Image Builder ===" -ForegroundColor Cyan
Write-Host ""

# Check if bootloader exists
if (-not (Test-Path $BootloaderPath)) {
    Write-Host "Error: Bootloader not found at $BootloaderPath" -ForegroundColor Red
    Write-Host "Please build the bootloader first: make bootloader" -ForegroundColor Yellow
    exit 1
}

# Verify bootloader is exactly 512 bytes
$bootloaderSize = (Get-Item $BootloaderPath).Length
if ($bootloaderSize -ne 512) {
    Write-Host "Error: Bootloader must be exactly 512 bytes" -ForegroundColor Red
    Write-Host "Current size: $bootloaderSize bytes" -ForegroundColor Red
    exit 1
}

Write-Host "✓ Bootloader verified: $bootloaderSize bytes" -ForegroundColor Green

# Create output directory if it doesn't exist
$outputDir = Split-Path -Parent $OutputImage
if (-not (Test-Path $outputDir)) {
    New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
}

# Calculate disk size in bytes
$diskSizeBytes = $DiskSizeKB * 1024

Write-Host "Creating disk image: $OutputImage ($DiskSizeKB KB)..." -ForegroundColor Cyan

# Create blank disk image
$blankImage = New-Object byte[] $diskSizeBytes
for ($i = 0; $i -lt $diskSizeBytes; $i++) {
    $blankImage[$i] = 0
}

# Write blank image to file
[System.IO.File]::WriteAllBytes((Resolve-Path -Path $OutputImage -ErrorAction SilentlyContinue) ?? (Join-Path (Get-Location) $OutputImage), $blankImage)

# Read bootloader binary
$bootloaderData = [System.IO.File]::ReadAllBytes((Resolve-Path $BootloaderPath))

# Write bootloader to first 512 bytes of disk image
$diskData = [System.IO.File]::ReadAllBytes((Resolve-Path $OutputImage))
for ($i = 0; $i -lt 512; $i++) {
    $diskData[$i] = $bootloaderData[$i]
}
[System.IO.File]::WriteAllBytes((Resolve-Path $OutputImage), $diskData)

Write-Host "✓ Disk image created successfully!" -ForegroundColor Green
Write-Host ""
Write-Host "Disk image: $OutputImage" -ForegroundColor White
Write-Host "Size: $DiskSizeKB KB ($diskSizeBytes bytes)" -ForegroundColor White
Write-Host ""
Write-Host "To test in QEMU, run:" -ForegroundColor Yellow
Write-Host "  qemu-system-i386 -drive format=raw,file=$OutputImage" -ForegroundColor White
Write-Host "Or simply:" -ForegroundColor Yellow
Write-Host "  make run" -ForegroundColor White
Write-Host ""
