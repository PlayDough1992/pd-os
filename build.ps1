param([string]$Target = "all")

# Use portable NASM
$nasmExe = "tools-download\nasm\nasm.exe"

if ($Target -eq "all" -or $Target -eq "run") {
    Write-Host "Building PD-OS..." -ForegroundColor Cyan
    if (-not (Test-Path build)) { mkdir build | Out-Null }
    
    & $nasmExe -f bin bootloader\stage1.asm -o build\bootloader.bin
    
    if ($LASTEXITCODE -eq 0 -and (Test-Path build\bootloader.bin)) {
        $size = (Get-Item build\bootloader.bin).Length
        Write-Host "[OK] Bootloader built: $size bytes" -ForegroundColor Green
        
        # Create disk image
        $blank = New-Object byte[] (1474560)
        [IO.File]::WriteAllBytes("$PWD\build\pd-os.img", $blank)
        $boot = [IO.File]::ReadAllBytes("$PWD\build\bootloader.bin")
        $disk = [IO.File]::ReadAllBytes("$PWD\build\pd-os.img")
        for ($i=0; $i -lt 512; $i++) { $disk[$i] = $boot[$i] }
        [IO.File]::WriteAllBytes("$PWD\build\pd-os.img", $disk)
        Write-Host "[OK] Disk image created" -ForegroundColor Green
        
        if ($Target -eq "run") {
            Write-Host "Starting QEMU..." -ForegroundColor Cyan
            & qemu-system-i386 -drive format=raw,file=build\pd-os.img -m 128M
        }
    } else {
        Write-Host "[FAIL] Build failed" -ForegroundColor Red
    }
} elseif ($Target -eq "clean") {
    if (Test-Path build) {
        Remove-Item -Recurse -Force build
        Write-Host "[OK] Cleaned" -ForegroundColor Green
    }
} else {
    Write-Host "PD-OS Build System" -ForegroundColor Cyan
    Write-Host "Usage: .\build.ps1 [all|run|clean]"
}
