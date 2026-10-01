# 근거 문서와 미확인 사항

아래 링크는 하드웨어 제조사, 운영체제 제작사, 구현 원저자의 원본 문서다. `IMPLEMENTATION_PLAN.md`의 상세 설계는 이 문서들에서 확인 가능한 사실과 프로젝트의 시험 가정을 구분해서 작성했다.

| 근거 | 확인한 사실 | 계획에 반영한 결정 |
| --- | --- | --- |
| [Intel 100/C230 PCH Datasheet Vol. 1](https://cdrdv2-public.intel.com/332690/332690_SKL%20PCH%20EDS%20H%20Book_rev007_PUBLIC.pdf), §29.1·§29.7 / §35.3·§35.7 | PCH SATA는 AHCI/RAID 모드만, IDE legacy 모드 없음. USB는 xHCI로 연결되고 EHCI 없음. USB 2.0 포트도 xHCI로 라우팅됨. | SATA용 AHCI와 입력/USB용 xHCI를 별도 필수 드라이버 트랙으로 설정. |
| [Intel AHCI 1.3.1 specification](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/serial-ata-ahci-spec-rev1-3-1.pdf), §2.1·§3.1·§3.3 | PCI BAR5의 ABAR, capability·port 레지스터, BIOS/OS handoff, command list 1KiB 및 FIS buffer 256B 정렬 등. | 먼저 레지스터 읽기와 DMA 버퍼 조건을 검증한 뒤 I/O 구현. |
| [Intel xHCI specification](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/extensible-host-controler-interface-usb-xhci.pdf), §5.3–5.5 / §6 | Capability/operational/runtime/doorbell 레지스터, command/event/transfer ring, slot·endpoint context와 이벤트 완료 모델. | 읽기 전용 capability probe 후 No-Op 완료, root hub, control/HID/bulk 순서. |
| [Microsoft, Differences in WDM Versions](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/differences-in-wdm-versions) 및 [IoIsWdmVersionAvailable](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-ioiswdmversionavailable) | Windows 98 WDM 버전은 1.00이며 NT 계열과 PnP·전원·kernel 지원에 차이가 있음. | 최신 NT 드라이버를 바이너리 이식하지 않고 Win98 DDK에 맞춘 로드·PnP 시험 선행. |
| [Microsoft, PORT_CONFIGURATION_INFORMATION](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/srb/ns-srb-_port_configuration_information) 및 [Adaptec 7800 드라이버 구성](https://storage.microsemi.com/en-us/support/scsi/2930/aha-2930cu/install_sw/7800_fms_for_98.htm?nc=%2Fen-us%2Fsupport%2Fscsi%2F2930%2Faha-2930cu%2Finstall_sw%2F7800_fms_for_98.htm) | SCSI 미니포트 인터페이스에 Win95/98 차이가 있으며 당시 Win95/98용 `.MPD` 배포 사례가 있음. | AHCI의 첫 운영체제 접점을 9x IOS/SCSIPORT 미니포트 후보로 설정. 정확한 DDK ABI는 실험 게이트. |
| [Microsoft, USB Host-Side Drivers](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/usb-3-0-driver-stack-architecture) | `usbport.sys` 중심의 USB 2.0 스택은 XP SP1 이후의 모델이고 USB 3.0/xHCI에는 별도 스택이 쓰임. | Win98 기본 설치에 xHCI 미니포트 장착 가능하다고 가정하지 않음. |
| [xHCI98 원저자의 구현 기록](https://yeokhengmeng.com/2026/08/xhci98-usb-host-driver/) | Win98에서 USB 2.0 상위 스택에 연결한 xHCI 미니포트 실험 사례. 작성자는 해당 미니포트 ABI가 공개 DDK 계약이 아니라고 밝힘. | 구현 가능성 참고용. 이 프로젝트 코드/바이너리에 포함하지 않고 자체 probe·로드/등록 시험으로 계약을 검증. |
| [ReactOS 저장소](https://github.com/reactos/reactos) | NT 구조의 오픈소스 운영체제이며 사용자 모드의 상당 부분을 Wine과 공유. | 알고리즘·프로토콜 참고와 출처 추적만 수행. Win9x 하부 ABI는 별도 구현. |

## 아직 확인되지 않은 사항

1. 특정 기준 보드의 CSM/레거시 부팅, PS/2 입력, 실제 INTx 라우팅과 AHCI/xHCI PCI ID.
2. 사용할 Windows 98 SE 이미지의 USB 상위 스택 구성과 합법적으로 배포 가능한 설치 방식.
3. Windows 98/2000 DDK의 실제 스토리지·USB 샘플이 선택한 빌드 환경에서 컴파일·로드되는지.
4. QEMU에서 확인된 AHCI/xHCI 동작이 선택한 Skylake PCH 리비전에서도 재현되는지.
5. Windows 98 설치 단계에서 AHCI 시스템 디스크를 접근할 수 있도록 하는 DOS/보호 모드 전환 경로.

이 항목들은 현재 **미달성 조건**이며, 각 게이트의 실험 기록이 생기기 전까지 드라이버 지원 범위에 넣지 않는다.
