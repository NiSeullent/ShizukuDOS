$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$out = Join-Path $root 'build'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$compiler = (Get-Command gcc -ErrorAction Stop).Source
$exe = Join-Path $out 'test_nvme_probe.exe'
& $compiler '-std=c89' '-O2' '-Wall' '-Wextra' '-Werror' '-pedantic' `
    (Join-Path $root 'nvme_probe.c') (Join-Path $root 'test_nvme_probe.c') `
    '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'NVMe host probe compile failed' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'NVMe host probe tests failed' }
