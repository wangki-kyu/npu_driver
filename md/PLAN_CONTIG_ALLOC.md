# PLAN — driver-allocated contiguous I/O buffer IOCTL

## 0. 한 줄 요약

지금: user가 `_aligned_malloc` → `IOCTL_MAP_BUFFER` 로 user-pinned page를 chip PTE 에 박는다.
바꾼 뒤: user가 `IOCTL_ALLOC_IO_BUFFERS` 한 방으로 driver 한테 "input VA = X, output VA = Y, scratch VA = Z 로 contiguous 잡아 달라" 요청 → driver 가 `MmAllocateContiguousMemorySpecifyCache` 로 4 KB-align kernel pool 잡고, chip PTE 에 박고, user-mode에 다시 매핑해서 user pointer 를 돌려준다.

목적은 두 가지.
1. user-pinned 4 KB page (paged-pool 기반) 가 host IOMMU 미등록이라 chip outbound write 가 reject 된다는 가설 해소 → driver가 직접 잡은 contiguous PA 면 host IOMMU 도 우회.
2. add_int8 outfeed 16 byte 가 host buffer 까지 정말 도달하는지 깨끗하게 확인.

---

## 1. 변경 전/후 흐름

### BEFORE (`add_model_test_console.cpp` 현재 코드)
```
user
  _aligned_malloc(INPUT_SIZE)  → pInputBuf      (paged-pool, 4 KB align)
  _aligned_malloc(OUTPUT_SIZE) → pOutputBuf
  _aligned_malloc(SCRATCH_SIZE)→ pScratchBuf
  IOCTL_MAP_BUFFER (input)   user_va → device VA 0x80000000_00000000
  IOCTL_MAP_BUFFER (output)  user_va → device VA 0x80000000_00002000
  ...
  IOCTL_INFER  (input/output user_va, device VA 모두 채워서 호출)
```
driver 입장에서 host buffer 는 **user 가 잡은 paged page** 다. 그 page 들은 host IOMMU 에 등록 안 돼 있어서 chip 이 outbound write 시도하면 host fabric 이 막을 수 있다.

### AFTER
```
user
  IOCTL_ALLOC_IO_BUFFERS {
      InputSize,   InputDeviceVA   = 0x80000000_00000000
      OutputSize,  OutputDeviceVA  = 0x80000000_00002000
      ScratchSize, ScratchDeviceVA = 0x80000000_00003000
  } → out {
      InputUserVA, OutputUserVA, ScratchUserVA   // driver 가 user proc 에 매핑한 ptr
      InputPa, OutputPa, ScratchPa               // 디버그용 PA (옵션)
  }
  ...input buffer 에 직접 write...
  IOCTL_INFER  (driver 가 device VA 로 PTE 이미 박아둠 → user_va 인자는 no-op or 검증용)
  ...output buffer 직접 read...
```
driver 가 잡는 메모리는 `MmAllocateContiguousMemorySpecifyCache` 로 4 GB 미만 PA 의 contiguous block. host IOMMU 우회 + chip PTE 정합 + cache attribute (`MmNonCached` or `MmCached + flush`) 우리가 통제.

---

## 2. Public.h 추가

```c
// IOCTL: driver-owned contiguous buffer allocation + chip PTE map + user mapping.
//   - 한 번에 input / output / scratch 셋 다 잡는다 (size 0 이면 skip)
//   - driver 가 잡은 kernel KVA 를 calling process user-mode 에 매핑해서 돌려준다
//   - chip PTE 는 caller 가 지정한 DeviceVA 에 박아준다
//   - 해제는 IOCTL_FREE_IO_BUFFERS or 디바이스 핸들 close 시 (FileCleanup)
#define IOCTL_ALLOC_IO_BUFFERS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_WRITE_ACCESS)

#define IOCTL_FREE_IO_BUFFERS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x807, METHOD_BUFFERED, FILE_WRITE_ACCESS)

typedef struct _IOCTL_ALLOC_IO_BUFFERS_IN {
    UINT64 InputSize;        // bytes (0 = skip)
    UINT64 InputDeviceVA;    // chip device VA (4 KB align). extended VA 권장.
    UINT64 OutputSize;
    UINT64 OutputDeviceVA;
    UINT64 ScratchSize;
    UINT64 ScratchDeviceVA;  // 0 + ScratchSize=0 이면 skip
} IOCTL_ALLOC_IO_BUFFERS_IN;

typedef struct _IOCTL_ALLOC_IO_BUFFERS_OUT {
    UINT64 InputUserVA;      // 호출한 process 의 user-mode VA
    UINT64 OutputUserVA;
    UINT64 ScratchUserVA;
    UINT64 InputPa;          // (디버그용) 첫 페이지 PA. contiguous 라 한 개로 충분.
    UINT64 OutputPa;
    UINT64 ScratchPa;
} IOCTL_ALLOC_IO_BUFFERS_OUT;
```

> 결정 1 — `METHOD_BUFFERED`: in/out 둘 다 작은 struct 라 BUFFERED 로 충분. user pointer 가 inout 으로 오가는 게 아니라 driver 가 만든 user VA 를 돌려주는 거라 `METHOD_OUT_DIRECT` 안 써도 된다.
>
> 결정 2 — DeviceVA 는 caller 지정: 기존 `IOCTL_MAP_BUFFER` 와 동일 모델 유지. layout 결정권을 user-side test code 가 갖는다 (libedgetpu trace 와 비교 디버깅 쉬움).
>
> 결정 3 — Scratch도 같은 IOCTL: 따로 떼면 호출 횟수만 늘고 lifetime 분리 이득은 없다 (셋 다 INFER 단위로 같이 살고 같이 죽음).

---

## 3. Driver 측 구현 — `Queue.c` 에 case 추가

### 3.1 device context 에 owner state 추가 (`Device.h`)

`DEVICE_CONTEXT` 안에 driver-allocated buffer 추적용 필드.

```c
typedef struct _ALLOC_IO_SLOT {
    PVOID   Kva;          // MmAllocateContiguousMemorySpecifyCache 결과
    PMDL    Mdl;          // user-map 용 MDL (IoAllocateMdl + MmBuildMdlForNonPagedPool)
    PVOID   UserVa;       // MmMapLockedPagesSpecifyCache(UserMode) 결과
    UINT64  DeviceVa;     // chip PTE 박은 위치
    SIZE_T  Size;         // 4 KB 배수
} ALLOC_IO_SLOT;

// DEVICE_CONTEXT 안에 추가:
ALLOC_IO_SLOT IoSlots[3];   // 0=input, 1=output, 2=scratch
```

> 인덱스 고정해두면 cleanup 단순. 셋 이상 필요해지면 그때 동적 배열로.

### 3.2 case 본체

`Queue.c` 의 `switch (IoControlCode)` 에 다음 case 추가. **IOCTL_PARAM_CACHE 위쪽** (예: `IOCTL_INFER` case 끝난 다음) 에 두면 디버깅 시 grep 동선이 깔끔.

```c
case IOCTL_ALLOC_IO_BUFFERS:
{
    PDEVICE_CONTEXT          pDC = DeviceGetContext(device);
    WDFMEMORY                inMem, outMem;
    IOCTL_ALLOC_IO_BUFFERS_IN  *pIn  = NULL;
    IOCTL_ALLOC_IO_BUFFERS_OUT *pOut = NULL;
    int i;

    if (InputBufferLength  < sizeof(*pIn) ||
        OutputBufferLength < sizeof(*pOut)) {
        status = STATUS_INVALID_PARAMETER;
        break;
    }
    status = WdfRequestRetrieveInputMemory(Request, &inMem);
    if (!NT_SUCCESS(status)) break;
    status = WdfRequestRetrieveOutputMemory(Request, &outMem);
    if (!NT_SUCCESS(status)) break;

    pIn  = (IOCTL_ALLOC_IO_BUFFERS_IN  *)WdfMemoryGetBuffer(inMem,  NULL);
    pOut = (IOCTL_ALLOC_IO_BUFFERS_OUT *)WdfMemoryGetBuffer(outMem, NULL);
    RtlZeroMemory(pOut, sizeof(*pOut));

    struct { UINT64 size, devVa; UINT64 *outUserVa, *outPa; } req[3] = {
        { pIn->InputSize,   pIn->InputDeviceVA,   &pOut->InputUserVA,   &pOut->InputPa   },
        { pIn->OutputSize,  pIn->OutputDeviceVA,  &pOut->OutputUserVA,  &pOut->OutputPa  },
        { pIn->ScratchSize, pIn->ScratchDeviceVA, &pOut->ScratchUserVA, &pOut->ScratchPa },
    };

    // 한 번에 셋 다 잡고 셋 다 매핑한다. 중간에 실패하면 이미 잡힌 거 전부 되돌림.
    for (i = 0; i < 3; i++) {
        ALLOC_IO_SLOT *slot = &pDC->IoSlots[i];
        SIZE_T size4k;
        PHYSICAL_ADDRESS lo, hi, none;
        PHYSICAL_ADDRESS pa;

        if (req[i].size == 0) continue;
        if (slot->Kva != NULL) {
            // 이미 잡혀있으면 명시적 free 강제. (재진입 방지)
            DbgPrint("[ALLOC_IO] slot %d already allocated — call IOCTL_FREE_IO_BUFFERS first\n", i);
            status = STATUS_DEVICE_BUSY;
            goto alloc_io_fail;
        }

        size4k = (SIZE_T)((req[i].size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
        lo.QuadPart   = 0;
        hi.QuadPart   = 0xFFFFFFFFLL;     // < 4 GB. chip MMU 가 32-bit PA 만 받으면 필수.
        none.QuadPart = 0;

        slot->Kva = MmAllocateContiguousMemorySpecifyCache(
            size4k, lo, hi, none, MmCached);
        if (slot->Kva == NULL) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto alloc_io_fail;
        }
        RtlZeroMemory(slot->Kva, size4k);
        slot->Size     = size4k;
        slot->DeviceVa = req[i].devVa;

        pa = MmGetPhysicalAddress(slot->Kva);

        // user-mode 매핑 — calling process 컨텍스트에서만 호출되므로 안전 (KMDF queue 콜백은 caller process 에서 직접 invoke).
        slot->Mdl = IoAllocateMdl(slot->Kva, (ULONG)size4k, FALSE, FALSE, NULL);
        if (slot->Mdl == NULL) {
            MmFreeContiguousMemory(slot->Kva); slot->Kva = NULL;
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto alloc_io_fail;
        }
        MmBuildMdlForNonPagedPool(slot->Mdl);   // contiguous-nonpaged 라 OK

        __try {
            slot->UserVa = MmMapLockedPagesSpecifyCache(
                slot->Mdl,
                UserMode,
                MmCached,
                NULL,
                FALSE,
                NormalPagePriority);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            slot->UserVa = NULL;
        }
        if (slot->UserVa == NULL) {
            IoFreeMdl(slot->Mdl); slot->Mdl = NULL;
            MmFreeContiguousMemory(slot->Kva); slot->Kva = NULL;
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto alloc_io_fail;
        }

        // chip PTE 박기 — ApexPageTableMap 안 거치고 여기서 직접 작성.
        // 핵심 가정 두 개:
        //   (a) 이 buffer 는 driver 가 contiguous 로 잡았다 → pa[i] = basePa + i*PAGE_SIZE
        //       (MDL/PFN 배열 필요 없음, MmProbeAndLockPages 도 불필요)
        //   (b) DeviceVa 는 caller 가 4 KB align 으로 줬다 (struct 주석에 명시)
        //
        // chip MMU layout (Hardware.h / Memory.c 기준):
        //   - simple VA       : DeviceVa < 0x8000_0000_0000_0000
        //                       chip PTE register file 에 직접 write
        //                       index = DeviceVa >> 12, 8 byte slot, valid bit = 0x1
        //                       MMIO offset = APEX_REG_PAGE_TABLE + idx*8
        //                       전체 entry 수 = pDC->PageTableSize  (보통 6144)
        //   - extended VA     : (DeviceVa & 0x8000_0000_0000_0000) 셋
        //                       2-level PT. chip PTE [6144 + subtableIdx] 는 init 시점에
        //                       ExtPool 의 4 KB sub-page 로 미리 박혀있음 (Memory.c §1).
        //                       우리는 그 sub-page (= host RAM 의 KVA) 안의 entry 만 쓰면 됨.
        //                       subtableIdx    = (DeviceVa >> 21) & 0x1FFF        (2 MB region)
        //                       hostTableStart = (DeviceVa >> 12) & 0x1FF         (subtable 안 4 KB index)
        //                       slot ptr       = ExtPoolKva + subtableIdx*PAGE_SIZE
        //                       slot[hostTableStart + i] = pa | 0x1
        {
            UINT64 baseDevVa = slot->DeviceVa;
            UINT64 basePa    = (UINT64)pa.QuadPart;
            UINT32 pageCount = (UINT32)(size4k >> PAGE_SHIFT);
            BOOLEAN isExtended = (baseDevVa & 0x8000000000000000ULL) != 0;
            UINT32 j;

            if ((baseDevVa & (PAGE_SIZE - 1)) != 0) {
                DbgPrint("[ALLOC_IO] slot %d DeviceVa=0x%llx not page-aligned\n", i, baseDevVa);
                MmUnmapLockedPages(slot->UserVa, slot->Mdl);
                IoFreeMdl(slot->Mdl); slot->Mdl = NULL;
                MmFreeContiguousMemory(slot->Kva); slot->Kva = NULL;
                slot->UserVa = NULL;
                status = STATUS_INVALID_PARAMETER;
                goto alloc_io_fail;
            }

            WdfSpinLockAcquire(pDC->PageTableLock);

            if (!isExtended) {
                // ---- simple VA: 1-level chip PTE register 직접 write ----
                UINT32 startPte = (UINT32)(baseDevVa >> PAGE_SHIFT);
                if (startPte + pageCount > pDC->PageTableSize) {
                    WdfSpinLockRelease(pDC->PageTableLock);
                    DbgPrint("[ALLOC_IO] slot %d simple-VA out of range: PTE[%u..%u] > %u\n",
                        i, startPte, startPte + pageCount - 1, pDC->PageTableSize);
                    MmUnmapLockedPages(slot->UserVa, slot->Mdl);
                    IoFreeMdl(slot->Mdl); slot->Mdl = NULL;
                    MmFreeContiguousMemory(slot->Kva); slot->Kva = NULL;
                    slot->UserVa = NULL;
                    status = STATUS_INVALID_PARAMETER;
                    goto alloc_io_fail;
                }
                for (j = 0; j < pageCount; j++) {
                    UINT64 pagePa = basePa + ((UINT64)j << PAGE_SHIFT);
                    apex_write_register(
                        pDC->Bar2BaseAddress,
                        APEX_REG_PAGE_TABLE + ((startPte + j) * 8),
                        pagePa | 0x1ULL);
                }
                DbgPrint("[ALLOC_IO] slot %d simple PTE[%u..%u] = (PA 0x%llx + i*4K) | 1\n",
                    i, startPte, startPte + pageCount - 1, basePa);
            } else {
                // ---- extended VA: 2-level — host-side subtable entry 만 write ----
                // chip PTE [6144 + subtableIdx] 는 init 시점에 박혀있다고 가정.
                if (pDC->ExtPoolKva == NULL) {
                    WdfSpinLockRelease(pDC->PageTableLock);
                    DbgPrint("[ALLOC_IO] slot %d extended path but ExtPoolKva == NULL\n", i);
                    MmUnmapLockedPages(slot->UserVa, slot->Mdl);
                    IoFreeMdl(slot->Mdl); slot->Mdl = NULL;
                    MmFreeContiguousMemory(slot->Kva); slot->Kva = NULL;
                    slot->UserVa = NULL;
                    status = STATUS_DEVICE_NOT_READY;
                    goto alloc_io_fail;
                }
                {
                    UINT32 subtableIdx    = (UINT32)((baseDevVa >> 21) & 0x1FFFu);
                    UINT32 hostTableStart = (UINT32)((baseDevVa >> 12) & 0x1FFu);
                    UINT64 *subtable;

                    // 한 번에 2 MB region 을 안 넘는다고 가정 (4 KB I/O buffer 라 사실상 항상 OK).
                    // 넘는 size 는 별도 ApexExtMapBuffer 식으로 split 필요.
                    if (hostTableStart + pageCount > 512u) {
                        WdfSpinLockRelease(pDC->PageTableLock);
                        DbgPrint("[ALLOC_IO] slot %d ext crosses 2 MB region (start=%u count=%u)\n",
                            i, hostTableStart, pageCount);
                        MmUnmapLockedPages(slot->UserVa, slot->Mdl);
                        IoFreeMdl(slot->Mdl); slot->Mdl = NULL;
                        MmFreeContiguousMemory(slot->Kva); slot->Kva = NULL;
                        slot->UserVa = NULL;
                        status = STATUS_INVALID_PARAMETER;
                        goto alloc_io_fail;
                    }
                    if (subtableIdx >= 2048u) {
                        WdfSpinLockRelease(pDC->PageTableLock);
                        status = STATUS_INVALID_PARAMETER;
                        goto alloc_io_fail;
                    }

                    subtable = (UINT64 *)((PUCHAR)pDC->ExtPoolKva +
                                          ((SIZE_T)subtableIdx << PAGE_SHIFT));
                    for (j = 0; j < pageCount; j++) {
                        UINT64 pagePa = basePa + ((UINT64)j << PAGE_SHIFT);
                        subtable[hostTableStart + j] = pagePa | 0x1ULL;
                    }
                    DbgPrint("[ALLOC_IO] slot %d ext VA=0x%llx -> sub[%u] hostIdx[%u..%u] = (PA 0x%llx + i*4K) | 1\n",
                        i, baseDevVa, subtableIdx,
                        hostTableStart, hostTableStart + pageCount - 1, basePa);
                }
            }

            WdfSpinLockRelease(pDC->PageTableLock);
            slot->DeviceVa = baseDevVa;   // unwind 에서 unmap 할 때 쓸 값
        }

        *req[i].outUserVa = (UINT64)slot->UserVa;
        *req[i].outPa     = (UINT64)pa.QuadPart;

        DbgPrint("[ALLOC_IO] slot %d: KVA=%p UserVA=%p PA=0x%llx DeviceVA=0x%llx size=0x%llx\n",
            i, slot->Kva, slot->UserVa, (UINT64)pa.QuadPart, slot->DeviceVa, (UINT64)size4k);
    }

    bytesReturned = sizeof(*pOut);
    status = STATUS_SUCCESS;
    break;

alloc_io_fail:
    // 부분 성공한 slot 전부 unwind. 같은 로직을 IOCTL_FREE_IO_BUFFERS 와 공유하기 위해
    // 별도 helper (FreeIoSlot(pDC, i)) 로 빼두는 게 깔끔.
    for (i = 0; i < 3; i++) {
        ALLOC_IO_SLOT *slot = &pDC->IoSlots[i];
        if (slot->UserVa) { MmUnmapLockedPages(slot->UserVa, slot->Mdl); slot->UserVa = NULL; }
        if (slot->Mdl)    { IoFreeMdl(slot->Mdl); slot->Mdl = NULL; }
        if (slot->Kva)    {
            ApexPageTableUnmap(device, slot->DeviceVa, slot->Size);
            MmFreeContiguousMemory(slot->Kva);
            slot->Kva = NULL; slot->Size = 0; slot->DeviceVa = 0;
        }
    }
    break;
}

case IOCTL_FREE_IO_BUFFERS:
{
    PDEVICE_CONTEXT pDC = DeviceGetContext(device);
    int i;
    for (i = 0; i < 3; i++) {
        ALLOC_IO_SLOT *slot = &pDC->IoSlots[i];
        if (slot->UserVa) { MmUnmapLockedPages(slot->UserVa, slot->Mdl); slot->UserVa = NULL; }
        if (slot->Mdl)    { IoFreeMdl(slot->Mdl); slot->Mdl = NULL; }
        if (slot->Kva)    {
            ApexPageTableUnmap(device, slot->DeviceVa, slot->Size);
            MmFreeContiguousMemory(slot->Kva);
            slot->Kva = NULL; slot->Size = 0; slot->DeviceVa = 0;
        }
    }
    status = STATUS_SUCCESS;
    break;
}
```

### 3.3 FileCleanup 안전망

user-mode 가 free 안 부르고 그냥 `CloseHandle` 해버려도 누수 안 나게 `EvtFileCleanup` (`Driver.c` 또는 `Device.c`) 에서 같은 unwind 돌린다.

```c
VOID NpuEvtFileCleanup(_In_ WDFFILEOBJECT FileObject)
{
    WDFDEVICE device = WdfFileObjectGetDevice(FileObject);
    PDEVICE_CONTEXT pDC = DeviceGetContext(device);
    int i;
    for (i = 0; i < 3; i++) {
        ALLOC_IO_SLOT *slot = &pDC->IoSlots[i];
        if (slot->UserVa) { MmUnmapLockedPages(slot->UserVa, slot->Mdl); slot->UserVa = NULL; }
        if (slot->Mdl)    { IoFreeMdl(slot->Mdl); slot->Mdl = NULL; }
        if (slot->Kva)    {
            ApexPageTableUnmap(device, slot->DeviceVa, slot->Size);
            MmFreeContiguousMemory(slot->Kva);
            slot->Kva = NULL; slot->Size = 0; slot->DeviceVa = 0;
        }
    }
}
```

> `MmUnmapLockedPages(UserMode)` 는 **buffer 를 매핑한 process 컨텍스트에서 호출되어야** 한다. KMDF `EvtFileCleanup` 은 closing process 컨텍스트에서 돌기 때문에 여기서 호출해야 안전. DriverUnload 시점에는 이미 process 가 죽었을 수 있어서 위험.

---

## 4. user-side — `add_model_test_console.cpp` 변경

### 4.1 제거되는 것
```cpp
pInputBuf  = AlignedAlloc4K(INPUT_SIZE);
pOutputBuf = AlignedAlloc4K(OUTPUT_SIZE);
pScratchBuf = (SCRATCH_SIZE > 0) ? AlignedAlloc4K(SCRATCH_SIZE) : nullptr;
```
그리고 input/output 용 `IOCTL_MAP_BUFFER` 호출 (현재 코드는 INFER 가 ii.InputDeviceVA / ii.OutputDeviceVA 만 채워줘서 별도 MAP 안 거는데, 이건 driver 가 INFER 시점에 자동으로 PTE 박는다는 가정. 새 IOCTL 로 옮기면 그 가정 자체가 사라지고 명시 매핑 됨).

### 4.2 추가되는 것

`main` 의 `// ioctl -> contiguous allocate` 주석 자리에 다음을 넣는다 (현재 코드의 `// Device handle` 블록 직후, INPUT_SIZE/OUTPUT_SIZE/SCRATCH_SIZE 산정한 다음).

```cpp
// ---- driver-allocated contiguous I/O buffers ----
IOCTL_ALLOC_IO_BUFFERS_IN  allocIn  = {};
IOCTL_ALLOC_IO_BUFFERS_OUT allocOut = {};
allocIn.InputSize     = INPUT_SIZE;
allocIn.InputDeviceVA  = VA_INPUT;
allocIn.OutputSize    = OUTPUT_SIZE;
allocIn.OutputDeviceVA = VA_OUTPUT;
allocIn.ScratchSize   = SCRATCH_SIZE;
allocIn.ScratchDeviceVA= (SCRATCH_SIZE > 0) ? VA_SCRATCH : 0;

if (!DeviceIoControl(handle, IOCTL_ALLOC_IO_BUFFERS,
                     &allocIn,  sizeof(allocIn),
                     &allocOut, sizeof(allocOut),
                     &bytesReturned, nullptr)) {
    std::cout << "[main] FAIL: IOCTL_ALLOC_IO_BUFFERS: " << GetLastError() << std::endl;
    goto cleanup;
}
pInputBuf   = (void*)allocOut.InputUserVA;
pOutputBuf  = (void*)allocOut.OutputUserVA;
pScratchBuf = (SCRATCH_SIZE > 0) ? (void*)allocOut.ScratchUserVA : nullptr;
std::cout << "[main] driver-allocated buffers:"
          << " input="   << pInputBuf   << " (PA 0x" << std::hex << allocOut.InputPa  << ")"
          << " output="  << pOutputBuf  << " (PA 0x"             << allocOut.OutputPa << ")"
          << " scratch=" << pScratchBuf << " (PA 0x"             << allocOut.ScratchPa<< ")"
          << std::dec << std::endl;
```

### 4.3 cleanup 변경

`cleanup:` 라벨 블록의 `AlignedFree(pInputBuf/pOutputBuf/pScratchBuf)` **제거**. 대신 device handle close 직전에 한 번:

```cpp
{
    DeviceIoControl(handle, IOCTL_FREE_IO_BUFFERS,
                    nullptr, 0, nullptr, 0, &bytesReturned, nullptr);
}
```
또는 `CloseHandle(handle)` 만으로도 (FileCleanup 에서) 정리되니 명시 호출은 옵션.

### 4.4 INFER 인자

`IOCTL_INFER_INFO` 의 `InputImageAddr` / `OutputBufferAddr` 는 driver 가 user_va 인지 kva 인지 헷갈리지 않게 **그대로 user_va (allocOut.\*UserVA)** 넘겨준다. 단, driver-allocated 임을 driver 가 인지하고 PTE 재매핑을 skip 해야 한다 — `ApexPageTableMap` 이 이미 `IoSlots[*].DeviceVa` 에 박아놨으니 INFER path 에서는 "이미 mapped 된 device VA 면 lock/map 단계 skip" 분기가 필요. 가장 단순한 구현:

> INFER 핸들러 진입부에서 `pDC->IoSlots[*]` 를 돌면서 `(ii.InputDeviceVA == slot->DeviceVa && (UINT64)slot->UserVa == ii.InputImageAddr)` 일치하면 "driver-owned, skip page-locking" flag 셋팅. 동일하게 output, scratch.

이 분기 추가가 부담되면 임시로 INFER 의 page-lock 블록을 통째 우회해도 된다 (test_console 전용 모드). 정공법은 위.

---

## 5. 캐시 모드 결정

| 옵션 | 장점 | 단점 |
|---|---|---|
| `MmCached` + 명시 flush | host CPU read/write 빠름 (memcmp, hex dump) | INFER 직전 `KeFlushIoBuffers` 또는 PTE 캐시 invalidate 필요 |
| `MmNonCached` | flush 신경 안 써도 됨, 가설 검증에는 가장 안전 | host CPU access 가 느림 (16 byte 라 무관) |
| `MmWriteCombined` | output 처럼 chip→host streaming write 에 유리 | input 쪽엔 이상하고 mixed 모드 복잡도 ↑ |

**add_int8 검증 1차 단계는 `MmNonCached` 권장**. cache coherency 변수 빼고 chip outbound write 가 host 까지 도달하는지만 본다. 통과한 뒤 `MmCached + flush` 로 옮기는 식.

---

## 6. 검증 체크리스트

빌드 후 단계별 확인.

- [ ] `[ALLOC_IO] slot 0/1/2: KVA=... UserVA=... PA=0x...` 세 줄이 DbgPrint 에 찍히고 PA 가 모두 < 4 GB
- [ ] user-side: `pInputBuf` 에 16 byte golden 쓴 후 다시 read 해서 정합 (user-mode → kernel buf round-trip 정상)
- [ ] PTE dump: chip PTE register `[VA_INPUT >> 12]`, `[VA_OUTPUT >> 12]`, `[VA_SCRATCH >> 12]` 가 `(PA | 1)` 으로 박혀 있음
- [ ] INFER 호출 후 `pOutputBuf` 가 zero 가 아님 (chip outbound write 가 host 까지 도달)
- [ ] 16 byte golden 비교 PASS
- [ ] `IOCTL_FREE_IO_BUFFERS` 후 다시 alloc 가능 (재진입성)
- [ ] FreeIoBuffers 안 부르고 `CloseHandle` 만 해도 누수 없음 (verifier 에서 leak 안 잡힘)

---

## 7. 작업 순서 (실제 type 따라 칠 때)

1. `include/Public.h` — IOCTL 코드 + 두 struct 추가
2. `npu_driver/Device.h` — `ALLOC_IO_SLOT` typedef + `DEVICE_CONTEXT` 멤버 추가
3. `npu_driver/Queue.c` — `IOCTL_ALLOC_IO_BUFFERS` / `IOCTL_FREE_IO_BUFFERS` case 추가, helper `FreeIoSlot()` 분리
4. `npu_driver/Driver.c` (또는 Device.c) — `EvtFileCleanup` 안에 누수 방지 unwind 추가
5. (선택) `Queue.c::IOCTL_INFER` — driver-owned slot 인지 검사하는 분기 추가
6. 빌드 → 드라이버 재서명 → 재로드
7. `add_model_test_console.cpp` 수정 — alloc/free 경로 새 IOCTL 로 갈아끼우기
8. 빌드 → 실행 → 위 체크리스트 순서대로 확인

---

## 8. 알려진 함정

- `MmAllocateContiguousMemorySpecifyCache` 는 IRQL `< DISPATCH_LEVEL` 에서만 호출. KMDF queue 콜백은 PASSIVE 라 OK.
- `MmMapLockedPagesSpecifyCache(UserMode)` 는 **caller process 컨텍스트** 에서 호출되어야 user 가 그 VA 를 쓸 수 있다. EvtIoDeviceControl 콜백은 caller process 에서 도는 게 일반적이지만, KMDF queue 가 `WdfIoQueueDispatchSequential` 로 다른 thread 로 dispatch 되면 캠퍼스 다른 process 컨텍스트로 빠질 수 있음 — queue config 확인 필수. 안전책: queue 를 `WdfIoQueueDispatchParallel` + `WdfDeviceConfigureRequestDispatching` 으로 calling thread 보존.
- `MmFreeContiguousMemory` 보다 먼저 `MmUnmapLockedPages(UserVa, Mdl)` 와 `IoFreeMdl(Mdl)` 가 와야 함. 순서 어기면 BSOD.
- 기존 `IOCTL_MAP_BUFFER` path 의 `LockedModelMdl` 같은 single-slot 상태와 충돌 안 나게 IoSlots 는 별도 영역 유지. cross 호출 막아라.
- chip PTE 영역: caller 가 던지는 `*DeviceVA` 가 extended VA (`0x8000_xxxx_xxxx_xxxx`) 인지 simple VA 인지에 따라 `ApexPageTableMap` 안에서 분기되어야 함 — 이미 존재하는 분기가 새 호출에서도 그대로 동작하는지 dump 로 확인. (memory 메모 `extended_pt_workplan` 참고)

---

## 9. 이게 통과하면 다음

- add_int8 byte-exact 통과 → ssd 디버깅으로 진입할 수 있는 oracle 확보 (`add_int8_oracle_strategy` 메모).
- 통과 못 하면 PA / DeviceVA / chip PTE register 셋 중 어디서 깨졌는지 좁혀서 `csr_diff_2026_05_04` 비교 재실행.
