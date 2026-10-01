# AHCI · xHCI 구현 계획 (Windows 98 SE, Intel 100-series PCH)

## 1. 범위와 판정 원칙

첫 실기기 목표는 **한 대의 특정 Skylake/100-series PC**다. 보드/노트북 모델, BIOS 버전, PCH SKU, SATA와 xHCI의 PCI vendor/device/revision ID, BAR 크기·위치, INTx 라우팅, CSM/레거시 부팅, PS/2 또는 별도 입력 경로를 기록한 뒤 해당 조합을 기준 플랫폼으로 고정한다. Intel 데이터시트의 기능 목록은 모든 보드가 부팅 경로와 레거시 입력 장치를 제공한다는 보증이 아니다.

각 단계는 **읽기 전용 탐색 → 초기화 → 장치 I/O → Win98 통합 → 재부팅·내구성** 순서로 진입한다. 성공 판정은 실제 Windows 98 SE 게스트 또는 실기기에서 관찰된 로그와 검사 결과로만 한다. ReactOS의 NT 드라이버와 Wine의 사용자 모드 코드는 설계 참조나 이식 후보일 뿐, Win98용 드라이버 완성 증거가 아니다.

### 필수 개발 환경

| 항목 | 요구사항 |
| --- | --- |
| Windows 98 SE | 별도로 설치한 게스트, 복구 가능한 원본 디스크 이미지, 안전 모드와 DOS 복구 경로 |
| 드라이버 도구 | Windows 98/2000 DDK의 해당 샘플·헤더·라이브러리를 확보해 **Win98 대상**으로 빌드. 최신 WDK/KMDF/UCX 사용을 전제하지 않음 |
| 호스트 실험 | AHCI·xHCI를 노출하는 QEMU 게스트, 별도 폐기 가능한 가상 디스크와 USB 이미지, 직렬 또는 파일 로그 |
| 실기기 실험 | 기준 보드, 되돌릴 수 있는 BIOS 설정, 백업된 부팅 매체, 시험용 SATA 디스크와 USB 메모리 |
| 회귀 기준 | 드라이버 설치 전 Win98 부팅·정상 종료, 장치 관리자 상태, 디스크 해시, 입력 장치 동작 기록 |

Windows 98/Me의 WDM 1.0은 NT 계열 WDM과 PnP·전원 IRP 및 지원 루틴이 다르다. 예를 들어 `IRP_MN_SURPRISE_REMOVAL`은 Win98에서 사용할 수 없다. 구현에 앞서 DDK 샘플을 Win98에서 로드·언로드하는 작은 수명주기 시험을 통과시킨다. [Microsoft, Differences in WDM Versions](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/differences-in-wdm-versions), [Microsoft, IoIsWdmVersionAvailable](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-ioiswdmversionavailable)

## 2. AHCI: 하드웨어와 Windows 98의 접점

100-series PCH SATA 컨트롤러는 AHCI/RAID 모드만 제공하며 레거시 IDE 모드가 없다. 이 트랙은 **AHCI 모드 한 개 컨트롤러, 한 개 SATA 디스크, 32비트 DMA 주소**로 시작한다. RAID·NCQ·핫플러그·전원 관리·ATAPI·부팅 디스크 지원은 차례로 확장한다. [Intel PCH 데이터시트, §29.1/§29.7](https://cdrdv2-public.intel.com/332690/332690_SKL%20PCH%20EDS%20H%20Book_rev007_PUBLIC.pdf)

### 최소 하드웨어 경로

1. PCI class/subclass/programming interface `01/06/01` 및 실제 보드의 PCI ID를 확인하고, PCI BAR5의 AHCI ABAR를 매핑한다. 범용 클래스만으로 무조건 바인딩하지 않고, 시험한 PCI ID와 리비전을 INF에 명시한다.
2. 읽기 전용으로 `CAP`, `PI`, `VS`, `CAP2`, `BOHC`, 해당 포트의 `PxSSTS/PxSIG/PxTFD`를 기록한다. 구현 포트가 실제 장착 포트인지 확인한다. [AHCI 1.3.1 §2.1, §3.1, §3.3](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/serial-ata-ahci-spec-rev1-3-1.pdf)
3. BIOS/OS 소유권 인계가 광고되면 규격의 `BOHC` 절차를 따라 인계한 뒤에만 제어 레지스터를 변경한다. 타임아웃·오류 로그와 중단 경로를 둔다.
4. DDK가 제공하는 DMA-safe 물리 메모리 할당·주소 변환을 사용한다. AHCI 포트당 명령 목록은 1KiB 정렬, 수신 FIS 버퍼는 256B 정렬이어야 한다. 커맨드 테이블/PRDT의 정렬과 4GiB 경계 조건을 별도 검증한다. 임의의 가상 포인터를 물리 주소로 캐스팅하지 않는다. [AHCI 1.3.1 §3.3.1–3.3.4](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/serial-ata-ahci-spec-rev1-3-1.pdf)
5. 포트 엔진을 정지·설정·재시작하는 규격 순서를 구현하고 `IDENTIFY DEVICE`부터 실행한다. AHCI 데이터 전송은 DMA를 사용한다. 첫 블록 I/O는 시험용 디스크의 읽기만 허용한다.
6. 폴링으로 명령 완료·타임아웃·오류 복구를 먼저 검증한 뒤 INTx 인터럽트를 연결한다. INTx가 실제 보드에 전달되는지 실측한다. MSI 또는 APIC 지원을 가정하지 않는다.

### Win98 통합 경로

Win9x는 NT용 `storport.sys` 경로가 아니다. 후보는 **Windows 98 IOS/SCSIPORT의 SCSI 미니포트(.MPD)** 경로다. Microsoft 문서는 Win95/98의 SCSI 미니포트 구성 차이를 언급하고, 당시 공급사 배포물도 Win95/98용 `.MPD`를 사용했다. 실제 DDK 샘플로 PCI 검색, DMA 주소, 요청(SRB) 완료, PnP/절전 호출 규약을 확인하기 전까지 미니포트 ABI를 확정하지 않는다. [Microsoft, PORT_CONFIGURATION_INFORMATION](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/srb/ns-srb-_port_configuration_information), [Adaptec 7800 Family Manager Set for 98](https://storage.microsemi.com/en-us/support/scsi/2930/aha-2930cu/install_sw/7800_fms_for_98.htm?nc=%2Fen-us%2Fsupport%2Fscsi%2F2930%2Faha-2930cu%2Finstall_sw%2F7800_fms_for_98.htm)

미니포트는 IOS 쪽의 SCSI 명령을 SATA ATA 명령으로 변환해야 한다. 최소 구현은 장치 탐색, `INQUIRY`, 용량 조회, 28/48-bit LBA의 `READ`와 `WRITE`, 캐시 flush, 오류·sense 응답, 매체 경계 검사다. 파일 시스템은 Windows의 기존 계층에 맡기며, 쓰기 I/O는 폐기 가능한 디스크에서만 시작한다.

**부팅 가능** 판정은 단순히 장치 관리자에 보이는 것과 다르다. 보호 모드 전환 시 드라이버 로드 순서, 설치 프로그램의 DOS 단계, 실제 시스템 볼륨의 읽기·쓰기, 콜드 부팅과 정상 종료가 모두 검증되어야 한다. AHCI 시스템 디스크 부팅은 가장 마지막 게이트다.

## 3. xHCI: 하드웨어와 Windows 98의 접점

100-series PCH는 xHCI가 USB 2.0/3.0 포트를 소유하고 EHCI가 없다. 우선 목표는 **USB 1.x/2.0 신호와 장치를 xHCI로 정상 처리**하는 것이다. SuperSpeed 처리와 USB 3.x 성능은 그다음 단계다. xHCI에 USB 2.0 버스가 연결된다는 사실과 Win98 USB 스택이 SuperSpeed를 이해한다는 것은 별개다. [Intel PCH 데이터시트, §35.3/§35.7](https://cdrdv2-public.intel.com/332690/332690_SKL%20PCH%20EDS%20H%20Book_rev007_PUBLIC.pdf), [Intel xHCI 규격](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/extensible-host-controler-interface-usb-xhci.pdf)

### 최소 하드웨어 경로

1. PCI class/subclass/programming interface `0C/03/30` 및 기준 보드의 PCI ID를 확인하고 BAR0 MMIO를 매핑한다. `CAPLENGTH/HCIVERSION`, `HCSPARAMS1`, `HCCPARAMS1`, `DBOFF`, `RTSOFF`, 확장 capability 목록과 레거시 BIOS 소유권 상태를 읽어 기록한다.
2. xHCI 규격에 맞춰 BIOS 소유권을 넘겨받고 컨트롤러를 정지·리셋한다. MMIO 접근은 BAR 길이와 정렬을 검사하고, 레지스터의 R/W1C 성격을 구분한다.
3. DCBAA, scratchpad, command ring, event ring/ERST, interrupter, device/endpoint context를 각 규격 정렬·경계에 맞게 할당한다. 32비트 DMA 또는 bounce buffer를 적용한다. 주소 범위와 ring cycle bit를 별도 검증한다.
4. `No-Op` 명령 완료 이벤트로 command/event ring을 입증한 후 루트 허브 포트 상태, 포트 전원, USB 2.0 장치 reset·slot 설정, 기본 제어 전송, descriptor 조회 순서로 진행한다.
5. 전송은 control → interrupt(HID) → bulk(대용량 저장소) → 허브/핫플러그 → isochronous → SuperSpeed 순서로 확장한다. 인터럽트 없는 폴링 단계와 실제 INTx 수신 단계의 로그를 분리한다.

### Win98 USB 스택 통합의 선행 게이트

Windows 98 SE 기본 설치의 USB 1.1 호스트 계층과, Windows 2000/XP 계열의 `usbport.sys` 기반 USB 2.0 계층은 동일한 확장 계약으로 취급하지 않는다. Microsoft의 현재 USB 2.0 포트/미니포트 설명은 XP SP1 이후의 스택을 대상으로 하므로, 그 구성요소가 Win98 기본 설치에 있다는 근거로 삼을 수 없다. 최근 Win98 xHCI 실험 구현은 별도 USB 2.0 스택에 미니포트로 연결했지만, 그 미니포트 ABI는 공개 DDK 계약이 아니라고 작성자가 밝힌다. 이 프로젝트에서는 **선택한 게스트의 상위 USB 스택과 호출 계약을 먼저 확인하는 로드/등록 실험**이 통과해야 하드웨어 미니포트 구현을 해당 경로로 진행한다. 실패하면 자체 USB 호스트·허브/클래스 연결 계층을 새로 구현해야 하며, 일정과 범위가 크게 증가한다. [Microsoft, USB Host-Side Drivers](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/usb-3-0-driver-stack-architecture), [xHCI98 작성자의 구현 기록](https://yeokhengmeng.com/2026/08/xhci98-usb-host-driver/)

설치 전 USB 키보드 동작은 BIOS 지원일 수 있다. 드라이버 시험에서는 Windows 시작 후 키보드/마우스가 **실제 xHCI 드라이버로 열거되었는지** 확인한다. PS/2 등 대체 입력·복구 경로 없이 실기기에서 첫 설치를 하지 않는다.

## 4. 단계와 실제 수락 시험

| 단계 | 산출물 | 통과 조건 |
| --- | --- | --- |
| D0 기준 시스템 | PCI ID/BAR/INTx/BIOS/OS 목록, Win98 기준 이미지 | 기준 게스트 3회 부팅·종료 성공, 시험 디스크/USB 이미지 해시 기록 |
| D1 읽기 전용 탐색 | 이 폴더의 probe를 Win98 진단 프로그램에서 호출 | ABAR/BAR0의 capability가 BIOS/호스트 PCI 도구와 일치, 범위 밖 MMIO 읽기 0회, 레지스터 쓰기 0회 |
| A1 AHCI 미니포트 수명주기 | Win98용 INF와 `.MPD` 시험 빌드 | 장치 관리자에서 대상 PCI 장치가 충돌 없이 바인딩, 시작/중지/재부팅 각 3회, 다른 디스크 변동 없음 |
| A2 AHCI 단일 디스크 읽기 | `IDENTIFY`, 읽기 명령, 타임아웃/오류 로그 | 가상 디스크의 처음·중간·끝 LBA 데이터가 호스트 해시와 일치, 무장치 포트 접근 시 제한 시간 안에 반환 |
| A3 AHCI 쓰기 | scratch 디스크만 허용하는 쓰기 경로 | 4KiB/64KiB/1MiB 경계·랜덤 I/O 후 전체 이미지 해시 또는 파일 내용 대조, 강제 종료 후 오류 복구 확인 |
| A4 AHCI 부팅 | 실제 AHCI 볼륨의 Win98 설치·부팅 | 안전한 복구 경로를 유지하면서 콜드 부팅 10회, 파일 시스템 검사 0건, 1시간 반복 읽기·쓰기 후 해시 일치 |
| X1 USB 스택 계약 | 상위 스택에 등록되는 무전송 xHCI 시험 바이너리 | Win98 SE에서 로드/언로드·콜백/자원 해제 관찰, 블루스크린 0건; 실패 시 아키텍처 재결정 |
| X2 xHCI 컨트롤러 | ring/인터럽터 초기화 | QEMU와 기준 보드에서 No-Op 완료 이벤트 반복 100회, 타임아웃·리셋 경로가 무한 대기 없이 종료 |
| X3 루트 허브·HID | USB 2.0 포트·장치 열거 | 재연결 100회에서 장치 ID 일치, 키보드/마우스 입력 30분 연속, 강제 분리 뒤 복구 |
| X4 USB 저장 장치 | control/interrupt/bulk, 허브 | 폐기 가능한 FAT32 USB 이미지의 파일 복사·삭제 100회와 호스트 해시 대조, 허브 경유·직결 모두 시험 |
| X5 회귀·전원 | 재부팅, 안전 모드, 절전(지원 시) | 콜드 부팅 10회, 장치 제거/재연결 및 정상 종료 반복에서 장치 관리자 오류·메모리 누수·파일 손상 없음 |

시험 결과에는 매번 **정확한 BIOS/게스트 이미지/드라이버 해시/PCI ID/로그/실패 재현 절차**를 남긴다. QEMU 통과는 실제 Skylake 보드 통과를 대신하지 않는다. 실기기 쓰기 시험은 부팅 디스크가 아닌 교체 가능한 대상에서 먼저 수행한다.

## 5. 당장 수행할 다음 작업

1. 기준 보드와 Win98 SE 게스트를 정하고, PCI 장치·BAR·INTx·BIOS/CSM 정보를 보존한다.
2. Win98/2000 DDK의 스토리지 미니포트와 USB 관련 샘플을 확보·빌드해 어떤 ABI가 실제 98에서 로드되는지 확인한다.
3. `probe/`의 OS 독립 함수를 Win98 진단 프로그램에 연결한다. 현재 코드는 MMIO를 매핑하지 않으며, 실제 하드웨어를 읽은 기록이 없다.
4. 두 드라이버의 INF와 빈 수명주기 빌드를 각각 만들되, 해당 단계가 통과하기 전에는 저장 장치·USB I/O 기능을 주장하지 않는다.
