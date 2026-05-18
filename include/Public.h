#ifdef INITGUID
DEFINE_GUID(GUID_DEVINTERFACE_npudriver,
    0x5202bf06, 0x5a10, 0x4c9f, 0xa3, 0xd0, 0x7d, 0x6b, 0xfb, 0xc3, 0xd6, 0x29);
#endif

// IOCTL codes for Coral APEX memory mapping
#define IOCTL_MAP_BUFFER \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_WRITE_ACCESS)

#define IOCTL_UNMAP_BUFFER \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_WRITE_ACCESS)

#define IOCTL_INFER \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_IN_DIRECT, FILE_WRITE_ACCESS)

#define IOCTL_PARAM_CACHE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_IN_DIRECT, FILE_WRITE_ACCESS)

// Inference using a previously cached PARAM_CACHE bitstream. Driver enqueues
// PARAM_CACHE descriptor + INFER descriptor back-to-back into the same IQ
// batch (libedgetpu pattern) so the chip's INFEED/PARAM engines never see an
// idle gap and never enter kHalted between the two. Same input struct as
// IOCTL_INFER — driver pulls the param bitstream addr/size from device context.
#define IOCTL_INFER_WITH_PARAM \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_IN_DIRECT, FILE_WRITE_ACCESS)

#define IOCTL_ALLOC_IO_BUFFERS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_WRITE_ACCESS)

#define IOCTL_FREE_IO_BUFFERS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x807, METHOD_BUFFERED, FILE_WRITE_ACCESS)

#define IOCTL_INFER_NEW \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x808, METHOD_IN_DIRECT, FILE_WRITE_ACCESS)

#define IOCTL_PARAM_CACHE_NEW \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x809, METHOD_BUFFERED, FILE_WRITE_ACCESS)

typedef struct _IOCTL_ALLOC_IO_BUFFERS_IN {
    UINT64 InputSize;       // bytes (0 = skip)
    UINT64 InputDeviceVA;       // chip device VA (4 KB align). extended VA 권장.
    // ★ 2026-05-18: OUTPUT 을 두 별도 slot 으로 분리 — libedgetpu 처럼.
    // bbox (Squeeze1) 와 score (convert_scores) 가 별도 PA range, 별도 base VA.
    UINT64 OutputBboxSize;
    UINT64 OutputBboxDeviceVA;
    UINT64 OutputScoreSize;
    UINT64 OutputScoreDeviceVA;
    UINT64 ScratchSize;
    UINT64 ScratchDeviceVA; // 0 + ScratchSize = 0 이면 skip
    UINT64 Exe0BitstreamSize;
    UINT64 Exe0BitstreamDeviceVA;
    // Phase 1 (PARAMETER_CACHING) 슬롯. STAND_ALONE 모델은 size=0 으로 두면 driver 가 skip.
    UINT64 ParamDataSize;        // exe1.parameters() blob (weights, ~MB 단위)
    UINT64 ParamDataDeviceVA;
    UINT64 Exe1BitstreamSize;    // PARAMETER_CACHING bitstream
    UINT64 Exe1BitstreamDeviceVA;
    UINT64 Exe0ParamSize;
    UINT64 Exe0ParamDeviceVA;
} IOCTL_ALLOC_IO_BUFFERS_IN;

typedef struct _IOCTL_ALLOC_IO_BUFFERS_OUT {
    UINT64 InputUserVA;      // 호출한 process 의 user-mode VA
    UINT64 OutputBboxUserVA;
    UINT64 OutputScoreUserVA;
    UINT64 ScratchUserVA;
    UINT64 Exe0BitStreamUserVA;
    UINT64 ParamDataUserVA;
    UINT64 Exe1BitstreamUserVA;
    UINT64 Exe0ParamUserVA;
    UINT64 InputPa;          // (디버그용) 첫 페이지 PA contiguous 라 한 개로 충분.
    UINT64 OutputBboxPa;
    UINT64 OutputScorePa;
    UINT64 ScratchPa;
    UINT64 Exe0BitstreamPa;
    UINT64 ParamDataPa;
    UINT64 Exe1BitstreamPa;
    UINT64 Exe0ParamPa;
} IOCTL_ALLOC_IO_BUFFERS_OUT;

// parameter caching (Phase 1): load weights into on-chip SRAM via PARAMETER_CACHING executable
typedef struct {
    UINT64 ParamAddr;     // userspace VA of exe1.parameters() data
    UINT64 ParamSize;     // size in bytes
    UINT64 ParamDeviceVA; // device VA for PTE mapping (PageAlignUp(exe1_bitstream_size))
    UINT64 BitstreamSize; // exe1 bitstream size (already mapped at device VA 0x0)
} IOCTL_PARAM_CACHE_INFO;

// model inference
typedef struct IOCTL_INFER_INFO {
    UINT64 InputImageAddr;    // userspace VA of input image
    UINT64 InputImageSize;    // size in bytes
    // ★ 2026-05-18: OUTPUT 두 별도 — libedgetpu 처럼 bbox/score 별도 base VA + PA range.
    UINT64 OutputBboxAddr;        // userspace VA of bbox (Squeeze1) output
    UINT64 OutputBboxSize;
    UINT64 OutputBboxDeviceVA;
    UINT64 OutputScoreAddr;       // userspace VA of score (convert_scores) output
    UINT64 OutputScoreSize;
    UINT64 OutputScoreDeviceVA;
    UINT64 InputDeviceVA;     // device VA for input PTE registration
    UINT64 BitstreamDeviceVA; // device VA of bitstream for Instr Queue descriptor (usually 0)
    UINT64 BitstreamSize;     // size of bitstream for Instr Queue descriptor
    UINT64 ScratchAddr;       // userspace VA of scratch buffer (0 if not needed)
    UINT64 ScratchSize;       // scratch size in bytes (0 if not needed)
    UINT64 ScratchDeviceVA;   // device VA for scratch PTE registration
} IOCTL_INFER_INFO;

// DMA direction for IOCTL_MAP_BUFFER / IOCTL_ALLOC_IO_BUFFERS.
// Mirrors libedgetpu's dma_data_direction (common_gasket_ioctl.inc:58-63)
// so wire-level semantics match coral.sys.
typedef enum _APEX_DMA_DIRECTION {
    APEX_DMA_BIDIRECTIONAL = 0,  // host ↔ device
    APEX_DMA_TO_DEVICE     = 1,  // input  (device reads host mem)
    APEX_DMA_FROM_DEVICE   = 2,  // output (device writes host mem)
    APEX_DMA_NONE          = 3
} APEX_DMA_DIRECTION;

// IOCTL input/output structures
typedef struct {
    UINT64 UserAddress;     // User buffer virtual address
    UINT64 Size;            // Size in bytes
    UINT64 DeviceAddress;   // Requested device VA (must be page-aligned). Driver writes
                            // PTE[DeviceAddress>>12 .. ] for the user pages.
    UINT32 Direction;       // APEX_DMA_DIRECTION — coral.sys 와 동일하게 page lock
                            // mode 결정 (IoReadAccess/IoWriteAccess/IoModifyAccess)
    UINT32 Reserved;        // align to 8B
} MAP_BUFFER_INPUT;

typedef struct {
    UINT64 DeviceAddress;   // Device virtual address to unmap
    UINT64 Size;            // Size in bytes
} UNMAP_BUFFER_INPUT;