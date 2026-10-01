# 읽기 전용 AHCI/xHCI capability 탐색 코드

`w98_hw_probe.c`는 호출자가 **이미 안전하게 매핑한 MMIO**를 읽는 콜백을 받아 규격의 기본 capability 필드만 해석한다. 하드웨어를 찾아내거나 MMIO를 직접 매핑하지 않으며, 레지스터 쓰기·DMA·인터럽트·운영체제 드라이버 진입점은 없다. 현재 실기기에서 실행한 이력도 없다.

Windows 98 진단 프로그램에 연결할 때는 다음을 먼저 구현한다.

1. Win98 DDK가 제공하는 PCI 자원 열거와 MMIO 매핑을 사용하고 실제 BAR 길이를 함수에 전달한다.
2. callback이 `volatile` 32비트 MMIO 읽기를 수행하도록 하되, 호출 전 디바이스 자원과 전원 상태를 확인한다.
3. 읽은 PCI ID, BAR, `w98_ahci_info` 또는 `w98_xhci_info`, 오류 코드를 직렬/파일 로그에 저장한다.
4. **실제 레지스터에 대한 쓰기 접근은 이 모듈에 추가하지 않는다.** 초기화는 별도 단계에서 규격의 소유권 인계·중단·리셋 절차를 검토한 후 작성한다.

호스트 단위 시험은 임의의 MMIO 값만 사용한다. `clang -std=c89 -Wall -Wextra -Werror -pedantic w98_hw_probe.c test_probe.c -o test_probe.exe`로 빌드할 수 있다. `test_probe.exe` 통과는 Windows 98 또는 물리 하드웨어 지원을 의미하지 않는다.

레지스터 근거: [Intel AHCI 1.3.1 §3.1](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/serial-ata-ahci-spec-rev1-3-1.pdf), [Intel xHCI §5.3](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/extensible-host-controler-interface-usb-xhci.pdf).
