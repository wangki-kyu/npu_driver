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


// model inference
typedef struct IOCTL_INFER_INFO {
    UINT64 InputImageAddr;    // userspace VA of input image
    UINT64 InputImageSize;    // size in bytes
    UINT64 OutputBufferAddr;  // userspace VA of output buffer
    UINT64 OutputBufferSize;  // size in bytes
    UINT64 InputDeviceVA;     // device VA for PTE registration (PAGE_ALIGN_UP(bitstream_size))
    UINT64 OutputDeviceVA;    // device VA for PTE registration (InputDeviceVA + PAGE_ALIGN_UP(InputImageSize))
    UINT64 BitstreamDeviceVA; // device VA of bitstream for Instr Queue descriptor (usually 0)
    UINT64 BitstreamSize;     // size of bitstream for Instr Queue descriptor
} IOCTL_INFER_INFO;

// IOCTL input/output structures
typedef struct {
    UINT64 UserAddress;  // User buffer virtual address
    UINT64 Size;         // Size in bytes
} MAP_BUFFER_INPUT;

typedef struct {
    UINT64 DeviceAddress;  // Device virtual address to unmap
    UINT64 Size;           // Size in bytes
} UNMAP_BUFFER_INPUT;