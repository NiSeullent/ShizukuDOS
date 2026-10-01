param(
    [ValidateSet('nem', 'hm')]
    [string]$Engine = 'nem'
)

# Hardware-backed engines only: NEM uses Windows Hypervisor Platform, HM uses
# VirtualBox's direct VT-x/AMD-V path. No interpreter/recompiler fallback.
$ErrorActionPreference = 'Stop'
$vbox = 'C:\Program Files\Oracle\VirtualBox\VBoxManage.exe'
if (-not (Test-Path -LiteralPath $vbox)) { throw 'VirtualBox 7.x is required.' }

$vmName = 'ShizukuDOS-Dev'
$vmBase = Join-Path $PSScriptRoot 'vbox'
$image = Join-Path $PSScriptRoot 'build\shizukudos.img'
$serial = Join-Path $vmBase 'serial.log'
$screenshot = Join-Path $vmBase 'boot.png'
New-Item -ItemType Directory -Force -Path $vmBase | Out-Null

$registered = & $vbox list vms
if ($LASTEXITCODE -ne 0) { throw 'Cannot list VirtualBox machines.' }
if (-not ($registered | Where-Object { $_ -match '^"ShizukuDOS-Dev"\s' })) {
    & $vbox createvm --name $vmName --basefolder $vmBase --ostype Other --register
    if ($LASTEXITCODE -ne 0) { throw 'Cannot create ShizukuDOS VirtualBox machine.' }
    & $vbox storagectl $vmName --name Floppy --add floppy
    if ($LASTEXITCODE -ne 0) { throw 'Cannot create floppy controller.' }
}

$vmInfo = & $vbox showvminfo $vmName --machinereadable
if ($LASTEXITCODE -ne 0) { throw 'Cannot read ShizukuDOS machine state.' }
if ($vmInfo -match 'VMState="running"') { throw 'ShizukuDOS-Dev is already running; stop it before rebuilding.' }
if (-not ($vmInfo -match 'storagecontrollername\d+="SATA"')) {
    & $vbox storagectl $vmName --name SATA --add sata --controller IntelAhci
    if ($LASTEXITCODE -ne 0) { throw 'Cannot create isolated AHCI probe controller.' }
}

& (Join-Path $PSScriptRoot 'build.ps1') | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'ShizukuDOS image build failed.' }

& $vbox modifyvm $vmName --memory 64 --cpus 1 --firmware bios --boot1 floppy --boot2 none --boot3 none --boot4 none --hwvirtex on --vm-execution-engine $Engine --nested-paging on --nic1 none --audio-enabled off --usb-ohci off --usb-ehci off --usb-xhci on --uart1 0x3F8 4 --uart-mode1 file $serial
if ($LASTEXITCODE -ne 0) { throw 'Cannot configure hardware-backed execution.' }
& $vbox storageattach $vmName --storagectl Floppy --port 0 --device 0 --type fdd --medium $image
if ($LASTEXITCODE -ne 0) { throw 'Cannot attach ShizukuDOS floppy image.' }
& $vbox startvm $vmName --type headless
if ($LASTEXITCODE -ne 0) { throw "VirtualBox hardware-backed '$Engine' start failed." }
$runningInfo = & $vbox showvminfo $vmName --machinereadable
if ($LASTEXITCODE -ne 0 -or -not ($runningInfo -match 'VMState="running"')) {
    throw 'The ShizukuDOS machine did not reach the running state.'
}

$log = Join-Path $vmBase 'ShizukuDOS-Dev\Logs\VBox.log'
$logText = Get-Content -LiteralPath $log -Raw
if ($Engine -eq 'nem' -and ($logText -notmatch 'NEM: Created partition' -or $logText -notmatch 'PGM: Enabling NEM mode' -or $logText -notmatch 'WHvCapabilityCodeHypervisorPresent is TRUE')) {
    throw 'The VirtualBox log did not confirm a Windows Hypervisor-backed NEM partition.'
}
if ($Engine -eq 'hm' -and $logText -notmatch 'HM: Using (VT-x|AMD-V) implementation') {
    throw 'The VirtualBox log did not confirm direct VT-x/AMD-V execution.'
}
$booted = $false
for ($attempt = 0; $attempt -lt 20; $attempt++) {
    if (Test-Path -LiteralPath $serial) {
        $serialText = Get-Content -LiteralPath $serial -Raw
        if ($serialText -match 'ShizukuDOS 0\.1' -and $serialText -match 'A:\\>') {
            $booted = $true
            break
        }
    }
    Start-Sleep -Milliseconds 500
}
if (-not $booted) { throw 'The virtual machine ran, but the ShizukuDOS shell did not appear.' }
& $vbox controlvm $vmName screenshotpng $screenshot
if ($LASTEXITCODE -ne 0) { throw 'Could not capture the VM screen.' }
Write-Output "ShizukuDOS is running in VirtualBox ($Engine)."
Write-Output "Boot screenshot: $screenshot"
Write-Output "Serial output: $serial"
Write-Output "VirtualBox execution log: $log"
Write-Output 'The VM includes empty AHCI and xHCI controllers for read-only PCI probing.'
