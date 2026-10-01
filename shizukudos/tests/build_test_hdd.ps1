$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$output = Join-Path $project 'build'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$nasm = (Get-Command nasm -ErrorAction Stop).Source
$python = (Get-Command python -ErrorAction Stop).Source
$mbr = Join-Path $output 'test_mbr.bin'
$vbr = Join-Path $output 'test_vbr.bin'
$disk = Join-Path $output 'test_hdd.img'
& $nasm -f bin (Join-Path $PSScriptRoot 'chainload_mbr.asm') -o $mbr
if ($LASTEXITCODE -ne 0) { throw 'Test MBR assembly failed.' }
& $nasm -f bin (Join-Path $PSScriptRoot 'chainload_vbr.asm') -o $vbr
if ($LASTEXITCODE -ne 0) { throw 'Test VBR assembly failed.' }
& $python (Join-Path $PSScriptRoot 'make_test_hdd.py') $mbr $vbr $disk
if ($LASTEXITCODE -ne 0) { throw 'Test hard disk build failed.' }
Write-Output $disk
