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
// AXI credit-shim status counters (R/O) — chip-internal AXI master channel stats.
// Used to determine whether the chip ever attempted outbound AXI writes.  If
// aw_insertion stays at 0 across an inference, the OUTFEED engine never even
// reached the AXI master phase; if both increment but host RAM stays unchanged,
// the writes went somewhere we can't see (PTE/IOMMU routing issue).
#define APEX_REG_AXI_AW_CREDIT_SHIM_INSERTION 0x48220
#define APEX_REG_AXI_W_CREDIT_SHIM_INSERTION  0x48260
#define APEX_REG_AXI_AW_CREDIT_SHIM_OCCUPANCY 0x48210
#define APEX_REG_AXI_W_CREDIT_SHIM_OCCUPANCY  0x48250

#define APEX_REG_PAGE_TABLE_SIZE            0x46000
#define APEX_REG_EXTENDED_TABLE             0x46008
// 0x46010 in gasket is APEX_BAR2_REG_KERNEL_HIB_TRANSLATION_ENABLE, but on this chip
// it reads back 0x1 at POR — INFEED/IQ/StatusBlock all use MMU translation correctly
// without us writing it, so we leave it alone.  (Tested: writing 1 made no difference
// to the OUTFEED-not-writing-host-RAM symptom.)
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
#define APEX_REG_KERNEL_HIB_PAGE_TABLE_INIT 0x46078  // poll != 0 after reset
#define APEX_REG_MSIX_TABLE_INIT            0x46080  // poll != 0 after reset

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
// RUN_CONTROL: kBeagleTileCsrOffsets (0x40000 base) — WRITE only
// RUN_STATUS:  kBeagleDebugTileCsrOffsets (0x42000 base) — READ only
#define APEX_REG_TILE_OP_RUN_CONTROL        0x400c0  // write: Tile Config CSR
#define APEX_REG_TILE_OP_RUN_STATUS         0x420e0  // read:  Tile Debug CSR
#define APEX_REG_TILE_OP_BREAKPOINT         0x420d0
#define APEX_REG_TILE_DEEP_SLEEP            0x42020  // Tile deep sleep delays

// ========== Tile Internal Communication ==========
// Narrow-to-Wide (N2W)
#define APEX_REG_NARROW_TO_WIDE_RUN_CONTROL 0x40150  // write: Tile Config CSR (was 0x42150 — WRONG)
#define APEX_REG_NARROW_TO_WIDE_RUN_STATUS  0x42158  // read:  Tile Debug CSR

// Wide-to-Narrow (W2N)
#define APEX_REG_WIDE_TO_NARROW_RUN_CONTROL 0x40110  // write: Tile Config CSR
#define APEX_REG_WIDE_TO_NARROW_RUN_STATUS  0x42118  // read:  Tile Debug CSR

// Mesh Bus (4 buses for inter-tile data transfer)
#define APEX_REG_MESH_BUS0_RUN_CONTROL      0x40250  // write: Tile Config CSR
#define APEX_REG_MESH_BUS0_RUN_STATUS       0x42258
#define APEX_REG_MESH_BUS1_RUN_CONTROL      0x40298
#define APEX_REG_MESH_BUS1_RUN_STATUS       0x422a0
#define APEX_REG_MESH_BUS2_RUN_CONTROL      0x402e0
#define APEX_REG_MESH_BUS2_RUN_STATUS       0x422e8
#define APEX_REG_MESH_BUS3_RUN_CONTROL      0x40328
#define APEX_REG_MESH_BUS3_RUN_STATUS       0x42330

// Ring Bus (Circular communication structure)
#define APEX_REG_RING_BUS_CONSUMER0_RUN_CONTROL  0x40190  // write: Tile Config CSR
#define APEX_REG_RING_BUS_CONSUMER0_RUN_STATUS   0x42198
#define APEX_REG_RING_BUS_CONSUMER1_RUN_CONTROL  0x401d0
#define APEX_REG_RING_BUS_CONSUMER1_RUN_STATUS   0x421d8
#define APEX_REG_RING_BUS_PRODUCER_RUN_CONTROL   0x40210
#define APEX_REG_RING_BUS_PRODUCER_RUN_STATUS    0x42218

// Tile Configuration (HIB broadcast: writes go to all tiles simultaneously)
#define APEX_REG_TILE_CONFIG0                0x48788  // beagle_csr_offsets.h: tileconfig0

// ========== Instruction Queue ==========
// Offsets from beagle_csr_offsets.h (libedgetpu kBeagleInstructionQueueCsrOffsets)
#define APEX_REG_INSTR_QUEUE_CONTROL         0x48568  // enable(bit0) | sb_wr_enable(bit2)
#define APEX_REG_INSTR_QUEUE_STATUS          0x48570  // poll bit0==1 after writing CONTROL
#define APEX_REG_INSTR_QUEUE_DESC_SIZE       0x48578  // descriptor size field
#define APEX_REG_INSTR_QUEUE_BASE            0x48590  // ring buffer device VA
#define APEX_REG_INSTR_QUEUE_STATUS_BLOCK    0x48598  // status block device VA
#define APEX_REG_INSTR_QUEUE_SIZE            0x485a0  // ring capacity (# descriptors)
#define APEX_REG_INSTR_QUEUE_TAIL            0x485a8  // cumulative descriptor count
#define APEX_REG_INSTR_QUEUE_FETCHED_HEAD    0x485b0  // hw fetch progress
// IQ_COMPLETED_HEAD (0x485b8) tracks IQ descriptor *fetch* progress only — it
// increments as soon as a descriptor is consumed from the ring, NOT when the
// inference pipeline (INFEED → compute → OUTFEED) finishes. The real "INFER
// done" signal is sc_host_int_count (0x486d0), incremented by SCALAR's
// host_interrupt 0 opcode placed AFTER the OUTFEED drain barrier. Use this
// register for diagnostics only — never for completion polling.
#define APEX_REG_INSTR_QUEUE_COMPLETED_HEAD  0x485b8  // IQ fetch progress (NOT inference completion)
#define APEX_REG_INSTR_QUEUE_INT_CONTROL     0x485c0  // interrupt control
#define APEX_REG_INSTR_QUEUE_INT_STATUS      0x485c8  // interrupt status (libedgetpu writes 0 to clear)

// ========== WIRE_INT_PENDING bit layout ==========
// Authoritative source: libedgetpu/driver/config/common_csr_helper.h
//   class WireIntBitArray (lines 395-446).  Same order as
//   driver/interrupt/interrupt_handler.h enum DW_INTERRUPT_*.
//
//   bit 0  (0x0001)  instruction_queue   — IQ descriptor fetched
//   bit 1  (0x0002)  input_actv_queue
//   bit 2  (0x0004)  param_queue
//   bit 3  (0x0008)  output_actv_queue
//   bit 4  (0x0010)  sc_host_0           ★ "INFER complete" (SCALAR host_interrupt 0)
//   bit 5  (0x0020)  sc_host_1
//   bit 6  (0x0040)  sc_host_2
//   bit 7  (0x0080)  sc_host_3
//   bit 8  (0x0100)  top_level_0
//   bit 9  (0x0200)  top_level_1
//   bit 10 (0x0400)  top_level_2
//   bit 11 (0x0800)  top_level_3
//   bit 12 (0x1000)  fatal_err           ★ HIB fatal error
//
//   pending = 0x0011 → IQ_INT + SC_HOST_0 (canonical "INFER done" handshake).
//   pending = 0x1000 → FATAL_ERR (NOT a completion signal, abort the IOCTL).
#define APEX_WIRE_BIT_IQ_INT             (1u << 0)   // 0x0001
#define APEX_WIRE_BIT_INPUT_ACTV_Q       (1u << 1)   // 0x0002
#define APEX_WIRE_BIT_PARAM_Q            (1u << 2)   // 0x0004
#define APEX_WIRE_BIT_OUTPUT_ACTV_Q      (1u << 3)   // 0x0008
#define APEX_WIRE_BIT_SC_HOST_0          (1u << 4)   // 0x0010
#define APEX_WIRE_BIT_SC_HOST_1          (1u << 5)   // 0x0020
#define APEX_WIRE_BIT_SC_HOST_2          (1u << 6)   // 0x0040
#define APEX_WIRE_BIT_SC_HOST_3          (1u << 7)   // 0x0080
#define APEX_WIRE_BIT_TOP_LEVEL_0        (1u << 8)   // 0x0100
#define APEX_WIRE_BIT_TOP_LEVEL_1        (1u << 9)   // 0x0200
#define APEX_WIRE_BIT_TOP_LEVEL_2        (1u << 10)  // 0x0400
#define APEX_WIRE_BIT_TOP_LEVEL_3        (1u << 11)  // 0x0800
#define APEX_WIRE_BIT_FATAL_ERR          (1u << 12)  // 0x1000
#define APEX_WIRE_BITS_SC_HOST_ANY       (0xFu << 4) // 0x00F0 — sc_host_0..3 mask
#define APEX_WIRE_BITS_TOP_LEVEL_ANY     (0xFu << 8) // 0x0F00 — top_level_0..3 mask

// ========== SC_HOST Interrupt Control ==========
#define APEX_REG_SC_HOST_INT_CONTROL        0x486a0  // write 0xF = enable all 4 SC_HOST completion interrupts (kNumInterrupts=4)
// SC_HOST_INT_STATUS / SC_HOST_INT_COUNT — REAL INFER completion signal.
// libedgetpu's canonical post-inference handshake (per host_queue.h + working
// runtime trace):
//   1) SCALAR reaches host_interrupt 0 opcode (after OUTFEED drain) → fires
//      SC_HOST_0 wire interrupt (WIRE_INT_PENDING bit 4 = 0x10).
//   2) ISR writes 0xE to SC_HOST_INT_STATUS to W1C-clear bits 1..3 (leaves
//      bit 0 alone — chip latches that one for the count register).
//   3) ISR reads SC_HOST_INT_COUNT — monotonic counter of SCALAR-issued
//      host_interrupt 0 events. Compare to last cached value to detect new
//      completion. Polling this register (not IQ_COMPLETED_HEAD) is what
//      guarantees OUTFEED has actually written results back to host RAM.
#define APEX_REG_SC_HOST_INT_STATUS         0x486a8  // W1C: write 0xE to ack SC_HOST 1..3
#define APEX_REG_SC_HOST_INT_COUNT          0x486d0  // monotonic SC_HOST_0 fire count (REAL INFER done)
#define APEX_REG_STATUS_BLOCK_UPDATE        0x486e8  // 0 = disable periodic status block auto-write
#define APEX_REG_TOP_LEVEL_INT_CONTROL      0x486b0  // write 0xF = enable top-level aggregator interrupts
#define APEX_REG_FATAL_ERR_INT_CONTROL      0x486c0  // write 1 = enable fatal-error interrupt
#define APEX_REG_DMA_BURST_LIMITER          0x487a8  // axi DMA burst limiter (write 0 explicitly)

// ========== MSI-X table inside BAR2 ==========
// Apex chip places its MSI-X table at BAR2+0x46800 instead of standard PCI capability space.
// Each entry is 16 bytes; mask bit lives at offset +12 of each entry.
// Linux gasket explicitly clears these mask bits because pci_enable_msix_exact does not
// (gasket_interrupt.c::force_msix_interrupt_unmasking, line 249).
// Windows KMDF does NOT touch this region — we must do it ourselves or no MSI-X TLP is emitted.
#define APEX_REG_KERNEL_HIB_MSIX_TABLE      0x46800
#define APEX_MSIX_VECTOR_SIZE               16
#define APEX_MSIX_MASK_BIT_OFFSET           12
#define APEX_INTERRUPT_COUNT                4   // we register 4 vectors

// ========== HIB credit registers (DebugHibUserCsrOffsets - never written by libedgetpu) ==========
// Hypothesis: GCB reset (RAM shutdown) wipes these the same way it wipes the
// MSI-X table at 0x46800.  POR default may be non-zero (host-default credits)
// but we have to verify by reading before/after reset.
#define APEX_REG_HIB_INSTRUCTION_CREDITS    0x48740
#define APEX_REG_HIB_INPUT_ACTV_CREDITS     0x48748
#define APEX_REG_HIB_PARAM_CREDITS          0x48750
#define APEX_REG_HIB_OUTPUT_ACTV_CREDITS    0x48758  // suspected outbound credit gate

// ========== Per-queue CSR (libedgetpu beagle_csr_offsets.h) ==========
// Beagle has 4 separate queues at the chip level: output_actv / instruction /
// input_actv / param. We use only the instruction queue, but reading the others'
// status/descriptor_size tells us what the chip expects.
//
// instruction_queue (libedgetpu uses this one):
#define APEX_REG_IQ_DESCRIPTOR_SIZE         0x48578  // RO: chip-expected element size
#define APEX_REG_IQ_MINIMUM_SIZE            0x48580
#define APEX_REG_IQ_MAXIMUM_SIZE            0x48588
// output_actv queue (chip-side OUTFEED queue we never touch):
#define APEX_REG_OUTQ_CONTROL               0x484f8
#define APEX_REG_OUTQ_STATUS                0x48508
#define APEX_REG_OUTQ_DESCRIPTOR_SIZE       0x48510
#define APEX_REG_OUTQ_MINIMUM_SIZE          0x48518
// input_actv queue:
#define APEX_REG_INQ_CONTROL                0x485d0
#define APEX_REG_INQ_STATUS                 0x485d8
#define APEX_REG_INQ_DESCRIPTOR_SIZE        0x485e0
#define APEX_REG_INQ_MINIMUM_SIZE           0x485e8
// param queue:
#define APEX_REG_PARAMQ_CONTROL             0x48638
#define APEX_REG_PARAMQ_STATUS              0x48640
#define APEX_REG_PARAMQ_DESCRIPTOR_SIZE     0x48648
#define APEX_REG_PARAMQ_MINIMUM_SIZE        0x48650

// ========== Internal arbiter performance counters (DebugHibUserCsrOffsets) ==========
// These counters reveal where outbound writes lose data inside the chip.
//
// write_request_arbiter — selects which outbound write source (OUTFEED data vs
// status block) gets the AXI write channel. If output_actv requests cycle but
// blocked dominates, AXI write channel can't drain OUTFEED.
#define APEX_REG_WRA_OUT_ACTV_REQ           0x48390
#define APEX_REG_WRA_OUT_ACTV_BLOCKED       0x48398
#define APEX_REG_WRA_STATUS_BLK_REQ         0x483b0  // status block (known-working path)
#define APEX_REG_WRA_STATUS_BLK_BLOCKED     0x483b8
//
// address_translation_arbiter — MMU walker arbitration. If output_actv request
// is non-zero but its translation cycles never advance, MMU never resolves
// OUTPUT VA → host PA.
#define APEX_REG_ATA_OUT_ACTV_REQ           0x48450
#define APEX_REG_ATA_OUT_ACTV_BLOCKED       0x48458
#define APEX_REG_ATA_INSTRUCTION_REQ        0x483d0  // baseline: instruction-fetch path
#define APEX_REG_ATA_INSTRUCTION_BLOCKED    0x483d8
#define APEX_REG_ATA_INPUT_ACTV_REQ         0x483f0  // baseline: INFEED works
#define APEX_REG_ATA_INPUT_ACTV_BLOCKED     0x483f8

// Top-level arbiter registers — single uint64 each (likely an enable mask
// or status word).  beagle_csr_offsets.h:868-870.
//
// Hypothesis: address_translation_arbiter (0x48728) has per-channel enable
// bits, and OUTFEED's bit is cleared so its requests bypass MMU translation
// (skipping it produces ata.out_actv.req=0 while wra.out_actv.req still
// counts the AXI write attempts).  Reading the value tells us whether
// OUTFEED has its translation enabled.
#define APEX_REG_READ_REQUEST_ARBITER       0x48718
#define APEX_REG_WRITE_REQUEST_ARBITER      0x48720
#define APEX_REG_ADDR_TRANSLATION_ARBITER   0x48728

// ========== Status & Debug ==========
#define APEX_REG_OMC0_D0                    0x01a0d0  // Temperature sensor
#define APEX_REG_USER_HIB_ERROR_STATUS      0x486f0  // HIB error status (beagle_csr_offsets.h)
#define APEX_REG_USER_HIB_ERROR_MASK        0x486f8  // HIB error mask
#define APEX_REG_USER_HIB_FIRST_ERROR       0x48700  // HIB first error status
#define APEX_REG_USER_HIB_FIRST_ERROR_TS    0x48708  // HIB first error timestamp
#define APEX_REG_SCALAR_CORE_ERROR_STATUS   0x41a0   // Scalar Core error status
#define APEX_REG_IDLEGENERATOR              0x4A000  // Idle state: 0xFFFFFFFF = all blocks idle
// 0x48738 = unified page_fault_address (libedgetpu beagle_csr_offsets.h:872).
// Captures faulting device VA for any direction (inbound INFEED read /
// outbound OUTFEED write / param walk). Name kept as INFEED_* for legacy.
#define APEX_REG_INFEED_PAGE_FAULT_ADDR     0x48738  // unified MMU fault VA

// ========== User HIB DMA Pause (GCB 리셋 전후 DMA 일시정지 제어) ==========
#define APEX_REG_USER_HIB_DMA_PAUSE     0x486D8  // write 1=pause, 0=unpause
#define APEX_REG_USER_HIB_DMA_PAUSED    0x486E0  // poll: (value & 1) == 1 = paused

// ========== System Control Unit (SCU) ==========
#define APEX_REG_SCU_BASE                   0x1A300
#define APEX_REG_SCU_CTRL_0                 (APEX_REG_SCU_BASE + 0x0C)  // PHY inactive mode bits
#define APEX_REG_SCU_2                      (APEX_REG_SCU_BASE + 0x14)
#define APEX_REG_SCU_3                      (APEX_REG_SCU_BASE + 0x18)
#define APEX_REG_AXI_QUIESCE                (APEX_REG_SCU_BASE + 0x2C)

// ========== CB Bridge (AXI bridge between Coral host and GCB) ==========
// libedgetpu beagle_csr_offsets.h: kBeagleCbBridgeCsrOffsets
// EnableReset 끝에 BULK credit pulse (0xF then 0x0) 가 필요. 안 하면 stale credit
// 으로 scalar 의 첫 DMA push 가 internal hang → INFEED 자동 halt.
#define APEX_REG_GCBB_CREDIT0               0x1907C  // BULK credit register

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

// Helper: Read-Modify-Write a 32-bit register field
// mask_width: number of bits, shift: starting bit position
static inline void apex_rmw_register_32(PVOID base, UINT64 offset, UINT32 value,
                                        UINT32 mask_width, UINT32 shift) {
    UINT32 mask = ((1U << mask_width) - 1) << shift;
    UINT32 current = apex_read_register_32(base, offset);
    UINT32 new_val = (current & ~mask) | ((value << shift) & mask);
    apex_write_register_32(base, offset, new_val);
}

#endif // _HARDWARE_H_