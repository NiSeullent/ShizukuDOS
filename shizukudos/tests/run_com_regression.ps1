# Exercises the real COM loader inside an isolated, hardware-backed VirtualBox VM.
$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$vbox = 'C:\Program Files\Oracle\VirtualBox\VBoxManage.exe'
$vmName = 'ShizukuDOS-Dev'
$serial = Join-Path $project 'vbox\serial.log'
$screenshot = Join-Path $project 'vbox\com-regression.png'
$started = $false

try {
    & (Join-Path $project 'run-vbox.ps1') | Out-Null
    $started = $true
    foreach ($command in @(
        'STACK',
        'EXEC STD.COM',
        'EXEC DEMO.COM',
        'EXEC RET.COM',
        'EXEC STD.COM',
        'STACK',
        'DIR',
        'TYPE HELLO.TXT',
        'PCI'
    )) {
        & $vbox controlvm $vmName keyboardputstring $command
        if ($LASTEXITCODE -ne 0) { throw "Failed to type: $command" }
        & $vbox controlvm $vmName keyboardputscancode 1c 9c
        if ($LASTEXITCODE -ne 0) { throw "Failed to submit: $command" }
        Start-Sleep -Milliseconds 350
    }
    & $vbox controlvm $vmName screenshotpng $screenshot
    if ($LASTEXITCODE -ne 0) { throw 'Could not capture the COM regression screen.' }

    $output = Get-Content -LiteralPath $serial -Raw
    $stack = [regex]::Matches($output, 'Shell SS:SP=([0-9A-F]{4}):([0-9A-F]{4})')
    if ($stack.Count -ne 2 -or $stack[0].Value -ne 'Shell SS:SP=6000:FFFE' -or $stack[1].Value -ne $stack[0].Value) {
        throw 'The shell stack is unsafe or changed across repeated EXEC calls.'
    }
    if ([regex]::Matches($output, 'STD forward string OK').Count -ne 2 -or
        [regex]::Matches($output, 'COM program exit code 0x33').Count -ne 2) {
        throw 'The DF=1 COM program did not run and return twice.'
    }
    foreach ($expected in @(
        'INT 21h version/error checks OK',
        'COM program exit code 0x2A',
        'COM program exit code 0x00',
        'HELLO   .TXT',
        'Hello from ShizukuDOS.',
        'AHCI (01:06:01):',
        'xHCI (0C:03:30):'
    )) {
        if (-not $output.Contains($expected)) { throw "Missing guest output: $expected" }
    }
    Write-Output "PASS: COM execution, DF preservation, shell stack $($stack[0].Value), and shell regressions."
    Write-Output "Guest output: $serial"
    Write-Output "Screenshot: $screenshot"
} finally {
    if ($started) {
        & $vbox controlvm $vmName poweroff | Out-Null
    }
}
