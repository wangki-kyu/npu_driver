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
// ========== Memory Management ==========
#define APEX_REG_PAGE_TABLE_SIZE            0x46000
#define APEX_REG_EXTENDED_TABLE             0x46008
#define APEX_REG_TRANSLATION_ENABLE         0x46010
#define APEX_REG_PAGE_TABLE                 0x50000

// ========== Interrupt Control ==========
#define APEX_REG_INSTR_QUEUE_INTVECCTL      0x46018
#define APEX_REG_INPUT_ACTV_QUEUE_INTVECCTL 0x46020
#define APEX_REG_PARAM_QUEUE_INTVECCTL      0x46028
#define APEX_REG_OUTPUT_ACTV_QUEUE_INTVECCTL 0x46030
#define APEX_REG_SC_HOST_INTVECCTL          0x46038
#define APEX_REG_TOP_LEVEL_INTVECCTL        0x46040
#define APEX_REG_FATAL_ERR_INTVECCTL        0x46048
#define APEX_REG_WIRE_INT_PENDING           0x48778
#define APEX_REG_WIRE_INT_MASK              0x48780
#define APEX_REG_DMA_PAUSE                  0x46050
#define APEX_REG_DMA_PAUSE_MASK             0x46058
#define APEX_REG_STATUS_BLOCK_DELAY         0x46060
#define APEX_REG_MSIX_PENDING_BIT_ARRAY0    0x46068
#define APEX_REG_MSIX_PENDING_BIT_ARRAY1    0x46070
#define APEX_REG_MSIX_TABLE_INIT            0x46080

// ========== Scalar Core (Instruction Processing) ==========
#define APEX_REG_SCALAR_RUN_CONTROL         0x44018
#define APEX_REG_SCALAR_RUN_STATUS          0x44258
#define APEX_REG_SCALAR_BREAKPOINT          0x44020

// ========== Data Feed Control (Input/Output) ==========
// Infeed (Input Data)
#define APEX_REG_INFEED_RUN_CONTROL         0x441d8
#define APEX_REG_INFEED_RUN_STATUS          0x441e0
#define APEX_REG_INFEED_BREAKPOINT          0x441e8

// Outfeed (Output Data)
#define APEX_REG_OUTFEED_RUN_CONTROL        0x44218
#define APEX_REG_OUTFEED_RUN_STATUS         0x44220
#define APEX_REG_OUTFEED_BREAKPOINT         0x44228

// Parameter Pop
#define APEX_REG_PARAMETER_POP_RUN_CONTROL  0x44198
#define APEX_REG_PARAMETER_POP_RUN_STATUS   0x441a8
#define APEX_REG_PARAMETER_POP_BREAKPOINT   0x441a0

// AV Data Pop (Activation Data)
#define APEX_REG_AVDATA_POP_RUN_CONTROL     0x44158
#define APEX_REG_AVDATA_POP_RUN_STATUS      0x44168
#define APEX_REG_AVDATA_POP_BREAKPOINT      0x44160

// ========== Tile Operation Control (Core Computation) ==========
#define APEX_REG_TILE_OP_RUN_CONTROL        0x420c0
#define APEX_REG_TILE_OP_RUN_STATUS         0x420e0
#define APEX_REG_TILE_OP_BREAKPOINT         0x420d0

// ========== Tile Internal Communication ==========
// Narrow-to-Wide (N2W)
#define APEX_REG_NARROW_TO_WIDE_RUN_CONTROL 0x42150
#define APEX_REG_NARROW_TO_WIDE_RUN_STATUS  0x42158

// Wide-to-Narrow (W2N)
#define APEX_REG_WIDE_TO_NARROW_RUN_CONTROL 0x42110
#define APEX_REG_WIDE_TO_NARROW_RUN_STATUS  0x42118

// Mesh Bus (4 buses for inter-tile data transfer)
#define APEX_REG_MESH_BUS0_RUN_CONTROL      0x42250
#define APEX_REG_MESH_BUS0_RUN_STATUS       0x42258
#define APEX_REG_MESH_BUS1_RUN_CONTROL      0x42298
#define APEX_REG_MESH_BUS1_RUN_STATUS       0x422a0
#define APEX_REG_MESH_BUS2_RUN_CONTROL      0x422e0
#define APEX_REG_MESH_BUS2_RUN_STATUS       0x422e8
#define APEX_REG_MESH_BUS3_RUN_CONTROL      0x42328
#define APEX_REG_MESH_BUS3_RUN_STATUS       0x42330

// Ring Bus (Circular communication structure)
#define APEX_REG_RING_BUS_CONSUMER0_RUN_CONTROL  0x42190
#define APEX_REG_RING_BUS_CONSUMER0_RUN_STATUS   0x42198
#define APEX_REG_RING_BUS_CONSUMER1_RUN_CONTROL  0x421d0
#define APEX_REG_RING_BUS_CONSUMER1_RUN_STATUS   0x421d8
#define APEX_REG_RING_BUS_PRODUCER_RUN_CONTROL   0x42210
#define APEX_REG_RING_BUS_PRODUCER_RUN_STATUS    0x42218

// Tile Configuration
#define APEX_REG_TILE_CONFIG0                0x48070

// ========== Status & Debug ==========
#define APEX_REG_OMC0_D0                    0x01a0d0  // Temperature sensor
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