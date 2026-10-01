# CSM NVMe read-only foundation

**Status:** PCI class discovery and controller capability decoding with host
mock tests **and a separate disposable VirtualBox BIOS guest observation**.
This is neither a Windows 98 block driver nor an installer.
It does not create admin/I/O queues, issue commands, enumerate namespaces,
perform DMA, service interrupts, expose INT 13h disks, or attach to Win98 IOS.
The BIOS guest saw VirtualBox's emulated NVMe controller. No physical NVMe
device or Windows 98 NVMe storage path has been tested through this code.

`nvme_probe.c` is original GPL-2.0-only C89 code based on the published
[PCI-SIG class code 01h:08h:02h](https://pcisig.com/PCIExpress/ECN/Base/AddClasstoDEVICE_INTERFACE_REPORT)
and the [NVM Express Base Specification](https://nvmexpress.org/specification/nvm-express-base-specification/)
controller property layout. The code reads PCI configuration dwords and three
NVMe MMIO dwords. It contains no I/O-port, PCI-config, or MMIO write path.

## Integration contract

- Supply `find_class` for one **bounded** class-match index `0..255`.
  ShizukuDOS's current `PCI` command already uses PCI BIOS `INT 1Ah` functions
  `B103h` (find class) and `B10Ah` (read config dword) for AHCI/xHCI. A DOS
  adapter could use those calls for NVMe after separately checking that the
  PCI BIOS service exists. A Win98 tool should use the 9x DDK's PCI resource
  enumeration path instead of assuming BIOS services survive protected mode.
- Supply a read-only PCI config callback. The parser confirms the returned
  class and endpoint header, reads vendor/device/revision and PCI command,
  and accepts only memory BAR0 (32 or 64 bit). It reads BAR1 only for a 64-bit
  BAR. It does **not** write all ones to a BAR to measure its size. The caller
  must obtain the assigned BAR length from its platform's resource manager.
- Only after the caller has mapped that actual PCI resource, supply a volatile
  32-bit MMIO read callback and the verified mapping length. The probe reads
  `CAP` low/high at `00h`/`04h` and `VS` at `08h`, then reports queue-entry
  limit, timeout units, doorbell stride, page-size bounds, and version. It
  refuses an unmapped/short region, disabled PCI memory decoding, and a BAR
  above 4 GiB until a suitable physical mapper has been validated. Output
  structures stay untouched on failure.

ShizukuDOS currently executes in real mode and its `PCI` command only lists
AHCI/xHCI IDs. It has no NVMe MMIO mapper; this module does not bypass that
gap. A CSM BIOS can discover an NVMe PCI function while having no NVMe INT 13h
boot service. Discovery and bootability must be tested separately. Windows 98
needs a dedicated storage driver, protected-mode resource mapping, queue/DMA
allocation, interrupt handling, namespace/block translation, and installation
path before NVMe storage is usable.

## Host test

```powershell
./drivers/nvme/build-test.ps1
```

The C89 test mocks PCI class search, config reads, and MMIO reads. It checks
valid 32/64-bit BARs and NVMe CAP/VS decoding, exactly three bounded register
reads, absent and misclassified devices, invalid BARs, read failures, disabled
memory decoding, above-4-GiB BARs, short mappings, malformed capabilities,
and unchanged outputs after failures. Passing this host test by itself does
**not** establish guest, emulator, or hardware operation. The independent
BIOS guest below tests the same discovery boundary on VirtualBox; it does not
link the C module into ShizukuDOS or Win98.

## Disposable BIOS guest observation

`guest/boot.asm` and `guest/stage.asm` form a standalone BIOS floppy. The
stage uses PCI BIOS `INT 1Ah` to find class `01:08:02` and **read** vendor,
device, class, command, and BAR0/BAR1 configuration fields. It never sizes a
BAR by writing all ones. It prints `NVME_READY` and waits for a host key. The
host harness queries VirtualBox's assigned PCI region using
[`VBoxManage debugvm info pci`](https://docs.oracle.com/en/virtualization/virtualbox/7.2/user/EN-VBOX-7-2-USER.pdf),
requires the exact BDF, Oracle emulated device ID, BAR base, and a bounded
region of at least 12 bytes to agree with the guest, and only then sends `Y`.
The binary itself also rejects any PCI ID other than VirtualBox's `80ee:4e56`.
The guest enters a flat 32-bit mode and reads only the NVMe `CAP` low/high
and `VS` dwords at offsets `00h`, `04h`, and `08h`. PCI config and NVMe MMIO
are never written. UART output and A20 setup are the only I/O-port writes
after the floppy's BIOS read. An unapproved or unknown controller does not
reach the MMIO reads. A real machine with no independently verified BAR
resource length must take that refusal path.

```powershell
python drivers/nvme/guest/build.py
python -m unittest discover -s drivers/nvme/guest -v
python drivers/nvme/guest/test_vbox.py
```

The harness creates uniquely named one-use BIOS VMs under the ignored
`drivers/nvme/build/vbox/` directory. It attaches a **controller with no
namespace disk** to the positive and host-denied cases, uses a separate
no-controller case, and removes only each verified disposable VM after saving
its serial transcript and current `VBox.log` acceleration line in local
`*-evidence.json`. It does not open an installed Windows 98 VM or modify
`shizukudos/stage2.asm`.

On this host, VirtualBox 7.2.18's debugger reported emulated
`80ee:4e56` at `00:0e.0` with BAR0 `F0408000..F040FFFF`, **32,768 bytes**.
That length is **host VirtualBox metadata, not a guest-measured BAR length**.
The guest independently read BDF `0070`, class/revision `01080200`, BAR base
`F0408000`, `CAP=00000020:0A010FFF`, and `VS=00010200`; the approved path
reported `PASS`. The current disposable VM log recorded `NEM: Created
partition`, identifying the WHPX/NEM hardware-accelerated path. Both the
host-denied and no-controller cases reported failure before any `NVME_MMIO`
line. The four host approval-gate tests reject mismatched BARs, devices,
missing region data, and short or implausible region lengths.

This is an emulated-controller read-only observation. It does not initialize
admin or I/O queues, perform DMA, issue NVMe commands, enumerate namespaces,
read or write NVMe blocks, provide BIOS INT 13h boot, or implement a Windows
98 driver or installer. Physical CSM/NVMe hardware remains untested.
