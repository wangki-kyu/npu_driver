# libedgetpu TPU 드라이버 심층 분석
> Google Coral Edge TPU (Beagle/DarwiNN) userspace 드라이버 분석  
> KMDF 및 커스텀 드라이버 작성 참고용

---

## 1. 전체 아키텍처 개요

```
애플리케이션
    │
    ▼
api::Driver  (api/driver.h)          ← 퍼블릭 인터페이스, 스레드-안전
    │
    ▼
driver::Driver  (driver/driver.h)    ← 상태 관리, 요청 스케줄링 (추상 기반 클래스)
    │
    ├── MmioDriver  (PCIe)           ← driver/mmio_driver.h
    │       │
    │       ├── KernelRegisters      ← BAR2 CSR 읽기/쓰기
    │       ├── HostQueue            ← 명령 큐 (DMA 기반 링 버퍼)
    │       ├── KernelMmuMapper      ← IOMMU 페이지 테이블 관리
    │       ├── InterruptController  ← CSR 기반 인터럽트
    │       └── CoherentAllocator   ← 커널 DMA 메모리
    │
    └── UsbDriver   (USB)            ← driver/usb/usb_driver.h
            │
            └── UsbMlCommands       ← Bulk 전송, 인터럽트 폴링
```

**핵심 원칙**: userspace 라이브러리이며, 커널 드라이버(gasket/apex)가 이미 로드된 상태에서
IOCTL 또는 libusb를 통해 하드웨어와 통신한다.

---

## 2. 디렉토리 구조

```
libedgetpu/
├── api/                        공개 API
│   ├── driver.h                최상위 Driver 인터페이스
│   └── buffer.h                Buffer 타입 정의
├── driver/
│   ├── driver.h / driver.cc    기반 드라이버 (상태 기계, 스케줄러)
│   ├── mmio_driver.h           PCIe MMIO 드라이버
│   ├── request.h               추론 요청 표현
│   ├── device_buffer.h         디바이스 VA 래퍼
│   ├── beagle/
│   │   ├── beagle_pci_driver_provider.cc       PCIe 드라이버 생성 (공통)
│   │   ├── beagle_pci_driver_provider_windows.cc  Windows 특화
│   │   └── beagle_pci_driver_provider_linux.cc    Linux 특화
│   ├── config/
│   │   └── beagle/beagle_chip_config.h         CSR 오프셋, 큐 설정
│   ├── interrupt/
│   │   ├── interrupt_controller.h              CSR 기반 인터럽트 제어
│   │   └── top_level_interrupt_manager.h       인터럽트 라우팅
│   ├── kernel/
│   │   ├── kernel_registers.h                  기반 레지스터 추상화
│   │   ├── kernel_mmu_mapper.h                 IOCTL 기반 MMU
│   │   ├── kernel_event_handler.h              이벤트 추상화
│   │   ├── gasket_ioctl.h                      IOCTL 번호 (플랫폼 선택)
│   │   ├── windows/
│   │   │   ├── windows_gasket_ioctl.inc        Windows IOCTL 정의
│   │   │   ├── kernel_registers_windows.h/.cc  DeviceIoControl 기반 BAR 매핑
│   │   │   ├── kernel_coherent_allocator_windows.h  DMA 메모리 IOCTL
│   │   │   └── kernel_event_handler_windows.h  Windows Event 객체
│   │   └── linux/
│   │       ├── linux_gasket_ioctl.inc          Linux ioctl 정의
│   │       └── kernel_registers_linux.cc       mmap 기반 BAR 매핑
│   ├── memory/
│   │   ├── mmu_mapper.h                        MMU 매핑 추상 인터페이스
│   │   ├── address_space.h                     디바이스 VA 공간 관리자
│   │   ├── dual_address_space.h                Simple + Extended PT 이중 공간
│   │   └── buddy_allocator.h                   버디 할당자 (VA 관리)
│   ├── mmio/
│   │   ├── host_queue.h                        명령 큐 (링 버퍼, 핵심)
│   │   └── coherent_allocator.h                Coherent DMA 메모리 추상화
│   ├── registers/
│   │   └── registers.h                         Read/Write/Poll 인터페이스
│   └── usb/
│       ├── usb_driver.h                        USB 드라이버
│       ├── usb_ml_commands.h                   ML 특화 USB 명령
│       └── usb_device_interface.h              libusb 추상화
└── tflite/                     TFLite 통합 (edgetpu_c.h 등 공개 API)
```

---

## 3. PCIe BAR 레이아웃 및 CSR 오프셋

**파일**: `driver/beagle/beagle_pci_driver_provider.cc:76-87`

Beagle TPU는 PCIe BAR2 를 CSR 공간으로 사용하며, 세 구역으로 나뉜다:

```
BAR2 오프셋      크기       용도
──────────────────────────────────────────────────────
0x40000         0x1000     Tile Config 0 CSR
0x44000         0x1000     Scalar Core CSR
0x48000         0x1000     User HIB (Host Interface Block)
```

**중요**: 각 섹션은 4KB 페이지 정렬 필요. KMDF에서는 `MmMapIoSpace` 또는
`WdfDeviceInitAssignWdmIrpPreprocessCallback`으로 BAR 매핑 후 섹션별 오프셋 접근.

---

## 4. Windows IOCTL 인터페이스

**파일**: `driver/kernel/windows/windows_gasket_ioctl.inc`

### 4.1 디바이스 이름 및 기본 상수

```c
// UMD가 드라이버를 열 때 사용하는 symbolic link 이름
#define CORAL_DOS_DRIVER_NAME  L"\\\\?\\ApexDriver"
#define APEX_DEVICE_NAME_BASE  "ApexDevice"
#define APEX_MAX_DEVICES       256

// CTL_CODE 파라미터
#define APEX_DRIVER_TYPE           43000   // DeviceType
#define APEX_DRIVER_IOCTL_ENUM_BASE 2833   // 함수 코드 베이스
```

### 4.2 공통 IOCTL 구조체

```c
// BAR2 뷰 매핑 / DMA 메모리 매핑에 공통으로 쓰이는 구조체
struct gasket_address_map_ioctl {
    uint64_t base_addr;     // 미사용 (항상 0)
    uint64_t offset;        // BAR2 내 오프셋
    uint64_t size;          // 매핑할 바이트 수
    uint64_t dev_dma_addr;  // 디바이스 DMA 물리 주소 (DMA 뷰에서 사용)
    uint32_t flags;         // 미사용 (항상 0)
    uint64_t* virtaddr;     // [OUT] 매핑된 사용자 공간 VA
};

// 인터럽트-이벤트 연결 구조체
struct gasket_set_event_ioctl {
    uint64_t int_num;           // 인터럽트 번호 (0-based)
    WCHAR event_name[MAX_PATH]; // CreateEvent로 만든 named event 이름
};
```

### 4.3 IOCTL 코드

```c
// ── BAR2 CSR 접근 ──────────────────────────────────────────────────────────
// gasket_address_map_ioctl.offset = CSR 섹션 오프셋 (0x40000, 0x44000, 0x48000)
// gasket_address_map_ioctl.size   = 0x1000 (4KB)
// 반환: gasket_address_map_ioctl.virtaddr → 사용자 공간 포인터
GASKET_IOCTL_MAP_HDW_VIEW         // BAR2 섹션 → 사용자 VA 매핑
GASKET_IOCTL_UNMAP_HDW_VIEW       // 매핑 해제

// ── Coherent DMA 메모리 접근 ───────────────────────────────────────────────
// 드라이버가 미리 할당한 coherent DMA 버퍼를 사용자 공간에 노출
// gasket_address_map_ioctl.dev_dma_addr = 디바이스 DMA PA (드라이버가 알려줌)
// 반환: gasket_address_map_ioctl.virtaddr → 사용자 공간 포인터
GASKET_IOCTL_MAP_UMDMA_VIEW       // Coherent DMA 버퍼 → 사용자 VA 매핑
GASKET_IOCTL_UNMAP_UMDMA_VIEW     // 매핑 해제
```

### 4.4 IOCTL 발행 방식 (userspace 래퍼)

**파일**: `driver/kernel/windows/windows_gasket_ioctl.inc:119-176`

```cpp
// UMD 내부에서 Linux ioctl() 시그니처를 모방한 인라인 래퍼
template <typename IPT>
int ioctl(FileDescriptor fd, ULONG ctlcode, IPT* ipt) {
    // METHOD_BUFFERED 방식만 지원
    // FILE_READ_DATA | FILE_WRITE_DATA → 입출력 동일 버퍼
    DeviceIoControl(fd, ctlcode,
                    ipt, sizeof(*ipt),   // InBuffer
                    ipt, sizeof(*ipt),   // OutBuffer (같은 버퍼)
                    NULL, NULL);
}
```

**실제 BAR 매핑 코드** (`kernel_registers_windows.cc:49-62`):
```cpp
StatusOr<uint64*> KernelRegistersWindows::MapRegion(
    FileDescriptor fd, const MappedRegisterRegion& region, bool read_only) {

    gasket_address_map_ioctl ioctl_data = {0, region.offset, region.size, 0, 0, nullptr};

    BOOL res = DeviceIoControl(fd,
                               GASKET_IOCTL_MAP_HDW_VIEW,
                               &ioctl_data, sizeof(ioctl_data),  // InBuffer
                               &ioctl_data, sizeof(ioctl_data),  // OutBuffer
                               NULL, NULL);
    if (!res) {
        return InternalError("gle=" + GetLastError());
    }
    // ioctl_data.virtaddr에 사용자 공간 포인터가 채워짐
    return static_cast<uint64*>(ioctl_data.virtaddr);
}
```

### 4.5 KMDF 커널 드라이버 측 구현 포인트

```
UMD가 GASKET_IOCTL_MAP_HDW_VIEW 보내면:
  1. 커널: IRP_MJ_DEVICE_CONTROL 수신
  2. offset, size로 BAR2 내 구간 특정
  3. MmMapLockedPagesSpecifyCache 또는 ZwMapViewOfSection으로
     해당 PA 구간을 요청 프로세스 VA에 매핑
  4. 매핑된 VA를 gasket_address_map_ioctl.virtaddr에 기록
  5. OutputBuffer 반환

UMD가 GASKET_IOCTL_MAP_UMDMA_VIEW 보내면:
  1. IRP_MJ_DEVICE_CONTROL 수신
  2. dev_dma_addr (= 커널이 이미 할당한 coherent 메모리의 PA)
  3. MmMapLockedPagesSpecifyCache로 해당 PA → 사용자 VA 매핑
  4. VA를 virtaddr에 기록 후 반환
```

---

## 5. 인터럽트 처리 (Windows)

**파일**: `driver/kernel/windows/kernel_event_handler_windows.h`

### 5.1 인터럽트-이벤트 연결 패턴

libedgetpu는 인터럽트를 **Named Event Object**로 추상화한다:

```
HW 인터럽트 발생
    │
    ▼
커널 ISR / DPC        (KMDF: EvtInterruptIsr → EvtInterruptDpc)
    │ SetEvent()
    ▼
Named Event Object    (CreateEvent로 생성, GASKET_IOCTL_SET_EVENT로 커널에 등록)
    │
    ▼
KernelEventWindows    (WaitForSingleObject 폴링 스레드)
    │ Handler callback
    ▼
HostQueue::ProcessStatusBlock()  (완료 처리)
```

### 5.2 이벤트 핸들러 인터페이스

```cpp
class KernelEventHandlerWindows : public KernelEventHandler {
    // 각 인터럽트 번호마다 호출됨
    FileDescriptor InitializeEventFd(int event_id) const override;
    // → CreateEvent(NULL, FALSE, FALSE, named_event_string) 반환

    Status SetEventFd(FileDescriptor fd, FileDescriptor event_fd,
                      int event_id) const override;
    // → DeviceIoControl(GASKET_IOCTL_SET_EVENT, {event_id, event_name})
    // → 커널이 해당 인터럽트 발생 시 이 이벤트를 Set

    Status ReleaseEventFd(FileDescriptor fd, FileDescriptor event_fd,
                          int event_id) const override;
    // → DeviceIoControl(GASKET_IOCTL_CLEAR_EVENT)

    std::unique_ptr<KernelEvent> CreateKernelEvent(
        FileDescriptor event_fd, KernelEvent::Handler handler) override;
    // → WaitForSingleObject 루프를 돌며 handler 호출하는 KernelEvent 생성
};
```

### 5.3 KMDF 커널 드라이버 측 구현 포인트

```
GASKET_IOCTL_SET_EVENT 수신 시:
  1. IrpStack에서 int_num과 event_name 추출
  2. ObReferenceObjectByName(event_name) → KEVENT 포인터 확보
  3. 해당 인터럽트 번호에 KEVENT 포인터 저장

EvtInterruptDpc (DPC 컨텍스트):
  1. 저장된 KEVENT에 KeSetEvent() 호출
  2. UMD의 WaitForSingleObject 깨어남
  3. UMD → HostQueue::ProcessStatusBlock() 실행
```

---

## 6. HostQueue – 명령 제출의 핵심

**파일**: `driver/mmio/host_queue.h`

### 6.1 구조

```
Host 메모리 (Coherent DMA)
┌─────────────────────────────────────────────────────┐
│  queue[0]  │  queue[1]  │ ... │  queue[size-1]      │  ← Element 배열
└─────────────────────────────────────────────────────┘

┌─────────────────┐
│  StatusBlock    │  completed_head_pointer, fatal_error
└─────────────────┘

CSR 레지스터:
  queue_base               → queue[] 의 디바이스 VA
  queue_status_block_base  → StatusBlock 의 디바이스 VA
  queue_size               → 256
  queue_tail               → 호스트가 쓴 마지막 인덱스 (write-only)
  queue_control            → enable 비트
  queue_status             → 활성화 여부 (poll target)
  queue_int_control        → 인터럽트 enable/disable
  queue_int_status         → 인터럽트 pending 클리어
```

### 6.2 Open 시퀀스 (`host_queue.h:271-331`)

```
1. coherent_allocator_->Open()
   → GASKET_IOCTL_MAP_UMDMA_VIEW 로 host queue 메모리 사용자 공간 노출

2. coherent_allocator_->Allocate(queue 크기)
3. coherent_allocator_->Allocate(StatusBlock 크기)

4. MapAll()
   → 각 버퍼를 MMU를 통해 디바이스 VA에 매핑
   → IOCTL: GASKET_IOCTL_MAP_BUFFER (Linux) / 페이지 테이블 업데이트

5. CSR 쓰기:
   Write(queue_base,              device_queue_buffer_.device_address())
   Write(queue_status_block_base, device_status_block_buffer_.device_address())
   Write(queue_size,              256)

6. CSR 쓰기: queue_control ← enable=1, sb_wr_enable=1
7. CSR Poll:  queue_status until enable 비트 세팅 확인
```

### 6.3 Enqueue 시퀀스 (`host_queue.h:373-393`)

```cpp
Status Enqueue(const Element& element, std::function<void(uint32)> callback) {
    // 1. 가용 공간 확인 (circular buffer)
    if (GetAvailableSpaceLocked() == 0) return UnavailableError;

    // 2. queue[tail_] = element  (DMA 메모리에 직접 씀)
    queue_[tail_] = element;
    callbacks_[tail_] = std::move(callback);

    // 3. tail 증가 (2^N 마스킹으로 wrap-around)
    ++tail_;
    tail_ &= (size_ - 1);

    // 4. queue_tail CSR 업데이트 → 하드웨어가 새 항목 인식
    Write(csr_offsets_.queue_tail, tail_);
}
```

### 6.4 완료 처리 시퀀스 (`host_queue.h:396-426`)

```cpp
void ProcessStatusBlock() {
    // 1. DMA 메모리의 StatusBlock 읽기
    StatusBlock sb = *status_block_;
    int completed_until = sb.completed_head_pointer;  // HW가 업데이트
    uint32 error_status = sb.fatal_error;

    // 2. completed_head_를 completed_until까지 전진하며 콜백 수집
    while (completed_head_ != completed_until) {
        dones.push_back(callbacks_[completed_head_]);
        ++completed_head_;
        completed_head_ &= (size_ - 1);
    }

    // 3. 인터럽트 pending 클리어
    Write(csr_offsets_.queue_int_status, 0);

    // 4. 콜백 실행 (에러 코드 전달)
    for (auto& done : dones) done(error_status);
}
```

---

## 7. MMU / IOMMU 매핑

**파일**: `driver/memory/mmu_mapper.h`, `driver/kernel/kernel_mmu_mapper.h`

### 7.1 계층 구조

```
MmuMapper (추상)
    └── KernelMmuMapper
            → GASKET_IOCTL_MAP_BUFFER    (Linux: ioctl)
            → GASKET_IOCTL_UNMAP_BUFFER
```

### 7.2 Linux IOCTL (참고)

```c
// Linux gasket 드라이버가 제공하는 IOMMU 관련 IOCTL
GASKET_IOCTL_MAP_BUFFER          // 호스트 버퍼를 디바이스 VA에 매핑
GASKET_IOCTL_UNMAP_BUFFER        // 언매핑
GASKET_IOCTL_NUMBER_PAGE_TABLES  // 페이지 테이블 개수 쿼리
GASKET_IOCTL_PAGE_TABLE_SIZE     // 페이지 테이블 크기
GASKET_IOCTL_SIMPLE_PAGE_TABLE_SIZE
GASKET_IOCTL_RESET               // 디바이스 리셋
GASKET_IOCTL_SET_EVENTFD         // Linux eventfd 기반 인터럽트
```

### 7.3 Simple vs Extended Page Table

```
SimplePageTable:   소수의 큰 연속 블록을 위한 빠른 1단계 매핑
ExtendedPageTable: 비연속 메모리를 위한 다단계 페이지 테이블 (HW MMU와 유사)

DualAddressSpace: 두 테이블을 같이 관리
  → Simple PT: HostQueue 버퍼, Coherent 메모리에 사용
  → Extended PT: 추론 입출력 버퍼 (비연속 가능)
```

---

## 8. USB 전송 인터페이스

**파일**: `driver/usb/usb_ml_commands.h`

### 8.1 엔드포인트 할당

```
방향       EP 번호   용도
─────────────────────────────────────────────
Bulk OUT    EP1      명령어 스트림 (Instructions)
Bulk OUT    EP2      입력 활성화 (Input Activations)
Bulk OUT    EP3      파라미터 (Weights)
Bulk IN     EP1      출력 활성화 (Output Activations)
Bulk IN     EP2      이벤트 디스크립터 (완료 알림)
Interrupt   EP3      인터럽트 알림
```

단일 엔드포인트 모드에서는 EP1 Bulk OUT 하나로 모든 OUT 트래픽 처리.
헤더(DescriptorTag + length)를 먼저 전송하여 타입 구분.

### 8.2 DescriptorTag

```cpp
enum class DescriptorTag {
    kInstructions     = 0,  // 실행 명령
    kInputActivations = 1,  // 입력 텐서
    kParameters       = 2,  // 가중치
    kOutputActivations = 3, // 출력 텐서
    kInterrupt0       = 4,  // 스칼라 코어 인터럽트 0
    kInterrupt1       = 5,
    kInterrupt2       = 6,
    kInterrupt3       = 7,
};
```

### 8.3 CSR 접근 (USB 경로)

USB 모드에서도 CSR 읽기/쓰기가 가능하다. Bulk OUT으로 특수 명령 전송:

```cpp
StatusOr<Register32> ReadRegister32(uint32_t offset);
StatusOr<Register64> ReadRegister64(uint32_t offset);
Status WriteRegister32(uint32_t offset, Register32 value);
Status WriteRegister64(uint32_t offset, Register64 value);
```

### 8.4 DFU (Device Firmware Upgrade)

```cpp
Status DfuDetach(int interface_number, uint16_t timeout_msec);
// → USB DFU class 표준 DETACH 요청 전송
// → 이후 디바이스가 DFU 모드로 재부팅되어 펌웨어 업데이트 가능
```

---

## 9. 드라이버 상태 기계 및 Open/Close 시퀀스

**파일**: `driver/driver.cc`

### 9.1 상태 전환

```
kClosed ──Open()──→ kOpen ──Close()──→ kClosing ──→ kClosed
                                  ↑                      │
                             에러 발생                    │
                                  └──────────────────────┘
```

### 9.2 Open 시퀀스 (`driver.cc:123-150`)

```
Driver::Open(debug_mode, context_lost)
│
├── state_mutex_ WriterLock 획득
├── num_clients_++ (이미 열린 경우 여기서 반환)
├── context_lost ? executable_registry_->ResetParametersLoaded()
├── DoOpen(debug_mode)  ← 구현 클래스 호출
│       │
│       ├── KernelRegisters::Open()
│       │       → CreateFile("\\\\.\\ApexDevice0")
│       │       → MapRegion() × 3   (Tile, ScalarCore, UserHIB)
│       │
│       ├── KernelMmuMapper::Open()
│       │       → GASKET_IOCTL_NUMBER_PAGE_TABLES / PAGE_TABLE_SIZE
│       │
│       ├── HostQueue::Open()        (CSR 설정 + coherent 메모리 매핑)
│       │
│       ├── InterruptController::EnableInterrupts()
│       │       → CSR Write(interrupt_enable_offset, 1)
│       │
│       └── Watchdog 시작
│
└── SetState(kOpen)
```

### 9.3 Close 시퀀스

```
Driver::Close(mode)
│
├── 진행 중인 요청 완료/취소 대기
├── DoClose()
│       ├── InterruptController::DisableInterrupts()
│       ├── HostQueue::Close()     (CSR disable + 메모리 언매핑)
│       ├── KernelMmuMapper::Close()
│       └── KernelRegisters::Close()  → CloseHandle / UnmapRegion
│
└── SetState(kClosed)
```

---

## 10. 요청 처리 흐름 (추론 실행)

```
api::Driver::Execute(request)
    │
    ├── 1. Request::Prepare()
    │       → 입출력 버퍼 검증
    │       → TpuRequest 생성 (배치 사이즈만큼)
    │
    ├── 2. MapBuffer()
    │       → MmuMapper::Map(buffer, device_va, direction)
    │       → IOCTL: GASKET_IOCTL_MAP_BUFFER
    │
    ├── 3. DmaScheduler::Submit(dma_infos)
    │       → DmaInfo 리스트: Instruction, InputActivation, Parameter, OutputActivation
    │
    ├── 4. HostQueue::Enqueue(descriptor, callback)
    │       → descriptor에 각 DMA 버퍼 디바이스 VA 기록
    │       → queue_tail CSR 업데이트
    │       → TPU가 큐에서 descriptor 읽어 실행 시작
    │
    ├── 5. 인터럽트 발생 (TPU → 커널 → Event Object → UMD)
    │
    ├── 6. HostQueue::ProcessStatusBlock()
    │       → StatusBlock.completed_head_pointer 확인
    │       → 완료 콜백 호출
    │
    └── 7. UnmapBuffer()
            → MmuMapper::Unmap()
```

---

## 11. 메모리 관리 패턴

**파일**: `api/buffer.h`, `driver/memory/`

### 11.1 Buffer 타입

```cpp
enum class Type {
    kWrapped      = 1,  // 사용자가 할당한 호스트 메모리 (핀 필요)
    kAllocated    = 2,  // 라이브러리가 malloc으로 할당
    kFileDescriptor = 3, // ION/DMA-BUF fd (Linux)
    kDram         = 4,  // 온칩 SRAM (TPU 내부)
    kDramWrapped  = 5,  // 외부 관리 온칩 메모리
};
```

### 11.2 버디 할당자 (디바이스 VA 관리)

```
BuddyAllocator (driver/memory/buddy_allocator.h)
  - 4KB 그래뉼러, 4KB 정렬
  - 2^N 크기 블록 단위로 VA 할당/해제
  - free_blocks_ / allocated_blocks_ 세트로 관리

사용 예:
  DualAddressSpace::Allocate(size) → BuddyAllocator에서 디바이스 VA 할당
  MmuMapper::Map(buffer, device_va) → 페이지 테이블에 실제 매핑 등록
```

### 11.3 Coherent DMA vs Streaming DMA

```
Coherent DMA (HostQueue, StatusBlock):
  - 커널이 할당한 physically contiguous, cache-coherent 메모리
  - CPU와 디바이스 모두 캐시 없이 접근 (WC/UC)
  - GASKET_IOCTL_MAP_UMDMA_VIEW로 사용자 공간에 노출
  - KMDF: WdfCommonBufferCreate / WdfCommonBufferGetAlignedVirtualAddress

Streaming DMA (입출력 텐서):
  - 일반 malloc 메모리를 DMA 전송 직전 핀 (pin)
  - IOCTL로 커널에 VA 전달 → 커널이 MDL 생성 및 페이지 핀
  - KMDF: WdfDmaTransactionCreate / IoAllocateMdl + MmProbeAndLockPages
```

---

## 12. 레지스터 읽기/쓰기 추상화

**파일**: `driver/registers/registers.h`

### 12.1 인터페이스

```cpp
class Registers {
    virtual Status Open();
    virtual Status Close();
    virtual StatusOr<uint64> Read(uint64 offset) = 0;
    virtual Status Write(uint64 offset, uint64 value) = 0;

    // CSR 비트 패턴이 expected와 같아질 때까지 대기
    virtual Status Poll(uint64 offset, uint64 expected_value,
                        uint64 mask = ~0ULL);
};
```

### 12.2 Windows 구현 (KernelRegistersWindows)

```
MapRegion() 호출 시:
  → GASKET_IOCTL_MAP_HDW_VIEW
  → 반환된 virtaddr 포인터 (uint64*) 저장

Read(offset):
  → mapped_region[offset / 8]  (직접 메모리 읽기)
  → 멀티 섹션 경우 offset으로 섹션 선택 후 섹션 내 오프셋으로 접근

Write(offset, value):
  → mapped_region[offset / 8] = value  (직접 메모리 쓰기)

Poll(offset, expected, mask):
  → while ((Read(offset) & mask) != expected) { sleep(1ms); }
  → 타임아웃 존재
```

---

## 13. BeaglePciDriverProvider – 드라이버 생성 전체 흐름

**파일**: `driver/beagle/beagle_pci_driver_provider.cc`

```cpp
StatusOr<unique_ptr<api::Driver>> BeaglePciDriverProvider::CreateDriver(
    const Device& device, const DriverOptions& options) {

    auto config = MakeUnique<BeagleChipConfig>();

    // ── 레지스터 (BAR2 CSR 매핑) ───────────────────────────────────────
    const vector<MmapRegion> regions = {
        {0x40000, 0x1000},  // TileConfig0
        {0x44000, 0x1000},  // ScalarCore
        {0x48000, 0x1000},  // UserHIB
    };
    auto registers = CreateKernelRegisters(device.path, regions, false);
    //   ↑ Windows: KernelRegistersWindows
    //   ↑ Linux:   KernelRegistersLinux (mmap 사용)

    // ── 인터럽트 핸들러 ────────────────────────────────────────────────
    auto interrupt_handler = CreateKernelInterruptHandler(device.path);
    //   ↑ Windows: KernelEventHandlerWindows + KernelInterruptHandler
    //   ↑ Linux:   eventfd + KernelInterruptHandler

    // ── MMU 매퍼 ──────────────────────────────────────────────────────
    auto mmu_mapper = MakeUnique<KernelMmuMapper>(device.path);

    // ── 주소 공간 (디바이스 VA 관리) ────────────────────────────────
    auto address_space = MakeUnique<DualAddressSpace>(
        config->GetChipStructures(), mmu_mapper.get());

    // ── Coherent DMA 할당자 (HostQueue용) ────────────────────────────
    constexpr int kCoherentAllocatorMaxSizeByte = 0x4000; // 16KB
    auto coherent_allocator = CreateKernelCoherentAllocator(
        device.path, alignment_bytes, kCoherentAllocatorMaxSizeByte);
    //   ↑ Windows: KernelCoherentAllocatorWindows (GASKET_IOCTL_MAP_UMDMA_VIEW)

    // ── 명령 큐 ───────────────────────────────────────────────────────
    constexpr int kInstructionQueueSize = 256;
    auto host_queue = MakeUnique<HostQueue<HostQueueDescriptor, HostQueueStatusBlock>>(
        config->GetInstructionQueueCsrOffsets(),
        config->GetChipStructures(),
        registers.get(),
        move(coherent_allocator),
        kInstructionQueueSize,
        /*single_descriptor_mode=*/false);

    // ── 인터럽트 컨트롤러 ────────────────────────────────────────────
    constexpr int kNumTopLevelInterrupts = 4;
    auto top_level_interrupt_controller =
        MakeUnique<DummyInterruptController>(kNumTopLevelInterrupts);
    auto fatal_error_interrupt_controller =
        MakeUnique<InterruptController>(
            config->GetFatalErrorInterruptCsrOffsets(), registers.get());

    // ── 스칼라 코어, 런 컨트롤러 ─────────────────────────────────────
    auto scalar_core_controller =
        MakeUnique<ScalarCoreController>(*config, registers.get());
    auto run_controller =
        MakeUnique<RunController>(*config, registers.get());

    // ── 최종 드라이버 생성 ────────────────────────────────────────────
    return MakeUnique<MmioDriver>(
        options, move(config), move(registers),
        move(dram_allocator), move(mmu_mapper),
        move(address_space), move(allocator), move(host_queue),
        move(interrupt_handler), move(top_level_interrupt_manager),
        move(fatal_error_interrupt_controller),
        move(scalar_core_controller), move(run_controller),
        move(top_level_handler), move(executable_registry),
        move(time_stamper));
}
```

---

## 14. KMDF 드라이버 작성 시 구현 체크리스트

### 14.1 디바이스 열거 및 열기

```
[ ] PCI 디바이스 식별
    Vendor ID: Google (0x1AE0)
    Device ID: Apex/Beagle
    Class: 0x0480 (ML Accelerator)

[ ] WdfDeviceCreate + WdfPdoInitSetEventCallbacks

[ ] BusType: PCIExpress
    WdfDeviceAssignS0IdleSettings / WdfDeviceAssignSxWakeSettings

[ ] Symbolic Link 생성
    IoCreateSymbolicLink(L"\\DosDevices\\ApexDevice0", ...)
    → UMD: CreateFile("\\\\.\\ApexDevice0", ...)
```

### 14.2 BAR 매핑 (GASKET_IOCTL_MAP_HDW_VIEW 구현)

```
[ ] WdfCmResourceListGetDescriptor로 BAR2 리소스 찾기

[ ] EvtDevicePrepareHardware:
    MmMapIoSpaceEx(bar2_pa, bar2_size, PAGE_READWRITE | PAGE_NOCACHE)
    또는 MmMapIoSpace(...)

[ ] IOCTL 핸들러 (GASKET_IOCTL_MAP_HDW_VIEW):
    1. 입력: gasket_address_map_ioctl.offset, .size
    2. bar2_va + offset → 해당 구간 PA 계산
    3. ZwMapViewOfSection 또는 MmMapLockedPagesSpecifyCache로
       요청 프로세스 VA에 매핑 (METHOD_NEITHER 방식이라면 직접)
       또는 MDL 생성 후 MmMapLockedPages(mdl, UserMode)
    4. 반환: gasket_address_map_ioctl.virtaddr = 사용자 VA
```

### 14.3 Coherent DMA 메모리 (GASKET_IOCTL_MAP_UMDMA_VIEW 구현)

```
[ ] EvtDevicePrepareHardware 또는 EvtDeviceAdd:
    WdfCommonBufferCreate(dmaEnabler, 0x4000, &commonBuffer)
    common_va = WdfCommonBufferGetAlignedVirtualAddress(commonBuffer)
    common_la = WdfCommonBufferGetAlignedLogicalAddress(commonBuffer)
    → common_la.QuadPart가 dev_dma_addr (디바이스가 볼 PA)

[ ] IOCTL 핸들러 (GASKET_IOCTL_MAP_UMDMA_VIEW):
    1. 입력: gasket_address_map_ioctl.dev_dma_addr
    2. dev_dma_addr에 해당하는 공통 버퍼 VA 조회
    3. MmBuildMdlForNonPagedPool(IoAllocateMdl(va, size, FALSE, FALSE, NULL))
    4. MmMapLockedPagesSpecifyCache(mdl, UserMode, MmCached, ...) → 사용자 VA
    5. 반환: virtaddr = 사용자 VA
```

### 14.4 인터럽트 처리 (GASKET_IOCTL_SET_EVENT 구현)

```
[ ] WdfInterruptCreate (MSI 또는 Line-based)
    EvtInterruptIsr: HW 인터럽트 클리어, DPC 스케줄
    EvtInterruptDpc: KeSetEvent(event_object) 호출

[ ] IOCTL 핸들러 (GASKET_IOCTL_SET_EVENT):
    1. 입력: gasket_set_event_ioctl.int_num, .event_name
    2. ObReferenceObjectByName(event_name, &KEVENT)
    3. interrupt_events[int_num] = kevent_ptr 저장

[ ] EvtInterruptDpc:
    if (interrupt_events[int_num])
        KeSetEvent(interrupt_events[int_num], IO_NO_INCREMENT, FALSE)

[ ] 디바이스 닫힐 때: ObDereferenceObject(kevent_ptr)
```

### 14.5 사용자 버퍼 핀닝 (추론 입출력 텐서)

```
[ ] IOCTL 핸들러 (GASKET_IOCTL_MAP_BUFFER 상당):
    1. 사용자 VA, size 수신
    2. IoAllocateMdl(user_va, size, FALSE, TRUE, irp)
    3. MmProbeAndLockPages(mdl, UserMode, IoModifyAccess)
    4. MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority)
    5. DmaAdapter->DmaOperations->MapTransfer(...)로 SG 리스트 생성
    6. 각 SG 항목의 PA를 TPU 페이지 테이블에 기록

[ ] 언매핑 시:
    MmUnlockPages(mdl) → IoFreeMdl(mdl)
    DmaAdapter->DmaOperations->FlushAdapterBuffersEx(...)
```

### 14.6 에러 처리

```
[ ] Watchdog 타이머: KeSetTimer로 추론 타임아웃 감지
    타임아웃 시 디바이스 리셋 (CSR Write) + 펜딩 IOCTL 취소

[ ] Fatal Error 인터럽트:
    EvtInterruptDpc에서 fatal 비트 확인
    → 모든 pending 요청 실패 처리
    → 드라이버 error 상태 진입
    → WDF IoInvalidateDeviceRelations 또는 디바이스 제거 요청

[ ] 디바이스 제거 시 (EvtDeviceReleaseHardware):
    모든 DMA 전송 중단
    MDL 언락
    공통 버퍼 해제 (WdfObjectDelete(commonBuffer))
    BAR 언매핑 (MmUnmapIoSpace)
```

---

## 15. 핵심 데이터 흐름 요약

```
[사용자 코드]
  ↓ CreateRequest / AddInput / AddOutput
[api::Driver]
  ↓ Submit(request, callback)
[driver::Driver]
  ↓ MapBuffer() → IOCTL → MDL 핀 → IOMMU 매핑
  ↓ HostQueue::Enqueue(descriptor)
    ↓ queue[tail] = {cmd_base, input_va, output_va, weight_va}
    ↓ Write(queue_tail_CSR, tail+1)  ← 이 순간 TPU가 실행 시작
[TPU 하드웨어]
  ↓ StatusBlock.completed_head++ 업데이트 (DMA write-back)
  ↓ 인터럽트 발생
[커널 드라이버]
  ↓ ISR → DPC → KeSetEvent(interrupt_event)
[UMD 워커 스레드]
  ↓ WaitForSingleObject 리턴
  ↓ HostQueue::ProcessStatusBlock()
    ↓ callbacks_[completed_head](error_status) 호출
[사용자 콜백]
  출력 텐서 사용 가능
```

---

## 16. 참고 파일 인덱스

| 주제 | 파일 |
|------|------|
| Windows IOCTL 정의 | `driver/kernel/windows/windows_gasket_ioctl.inc` |
| Windows BAR 매핑 구현 | `driver/kernel/windows/kernel_registers_windows.cc` |
| Windows 이벤트 핸들러 | `driver/kernel/windows/kernel_event_handler_windows.h` |
| Windows Coherent 할당자 | `driver/kernel/windows/kernel_coherent_allocator_windows.h` |
| Windows PCIe 드라이버 생성 | `driver/beagle/beagle_pci_driver_provider_windows.cc` |
| PCIe BAR 오프셋 | `driver/beagle/beagle_pci_driver_provider.cc:76-87` |
| 명령 큐 전체 구현 | `driver/mmio/host_queue.h` |
| MMU 매퍼 인터페이스 | `driver/memory/mmu_mapper.h` |
| 인터럽트 컨트롤러 | `driver/interrupt/interrupt_controller.h` |
| USB 엔드포인트 / 명령 | `driver/usb/usb_ml_commands.h` |
| 드라이버 상태 기계 | `driver/driver.h`, `driver/driver.cc` |
| 드라이버 최종 조립 | `driver/beagle/beagle_pci_driver_provider.cc` |
| Buffer 타입 정의 | `api/buffer.h` |
| 칩 설정 (CSR 오프셋) | `driver/config/beagle/beagle_chip_config.h` |
```
