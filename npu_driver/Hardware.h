#pragma once
#include <ntddk.h>

// hardware.h
#ifndef _HARDWARE_H_
#define _HARDWARE_H_

// PCI info
#define APEX_PCI_VENDOR_ID      0x1ac1
#define APEX_PCI_DEVICE_ID      0x089a
#define APEX_BAR_INDEX          2      // BAR2 CSR

// BAR2 length
#define APEX_BAR_BYTES          0x100000  // 1MB

// register offset (BAR2)
#define APEX_REG_PAGE_TABLE_SIZE            0x46000
#define APEX_REG_EXTENDED_TABLE             0x46008
#define APEX_REG_TRANSLATION_ENABLE         0x46010
#define APEX_REG_INSTR_QUEUE_INTVECCTL      0x46018
#define APEX_REG_INPUT_ACTV_QUEUE_INTVECCTL 0x46020
#define APEX_REG_PARAM_QUEUE_INTVECCTL      0x46028
#define APEX_REG_OUTPUT_ACTV_QUEUE_INTVECCTL 0x46030
#define APEX_REG_SC_HOST_INTVECCTL          0x46038
#define APEX_REG_TOP_LEVEL_INTVECCTL        0x46040
#define APEX_REG_FATAL_ERR_INTVECCTL        0x46048
#define APEX_REG_DMA_PAUSE                  0x46050
#define APEX_REG_DMA_PAUSE_MASK             0x46058
#define APEX_REG_STATUS_BLOCK_DELAY         0x46060
#define APEX_REG_MSIX_PENDING_BIT_ARRAY0    0x46068
#define APEX_REG_MSIX_PENDING_BIT_ARRAY1    0x46070
#define APEX_REG_MSIX_TABLE_INIT            0x46080
#define APEX_REG_PAGE_TABLE                 0x50000
#define APEX_REG_WIRE_INT_PENDING           0x48778
#define APEX_REG_WIRE_INT_MASK              0x48780
#define APEX_REG_OMC0_D0                    0x01a0d0  // temp
#define APEX_REG_ERROR_STATUS               0x86f0

// page table
#define APEX_PAGE_TABLE_ENTRIES    8192
#define APEX_EXTENDED_BIT          63

// device status
#define DEVICE_STATUS_DEAD         0
#define DEVICE_STATUS_LAMED        1
#define DEVICE_STATUS_ALIVE        2
#define DEVICE_STATUS_EXITING      3

// utils function
static inline UINT64 apex_read_register(PVOID base, UINT64 offset) {
    return *(volatile UINT64*)((PUCHAR)base + offset);
}

static inline void apex_write_register(PVOID base, UINT64 offset, UINT64 value) {
    *(volatile UINT64*)((PUCHAR)base + offset) = value;
    // memory barrier
    KeMemoryBarrier();
}

static inline UINT32 apex_read_register_32(PVOID base, UINT64 offset) {
    return *(volatile UINT32*)((PUCHAR)base + offset);
}

static inline void apex_write_register_32(PVOID base, UINT64 offset, UINT32 value) {
    *(volatile UINT32*)((PUCHAR)base + offset) = value;
    KeMemoryBarrier();
}

#endif // _HARDWARE_H_