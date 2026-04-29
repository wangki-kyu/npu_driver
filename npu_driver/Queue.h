#pragma once
#include <wdf.h>

EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL npudriverEvtIoDeviceControl;

// PCI Config-space diagnostic — prints Command(BME/MEM/IO) + Status(MasterAbort/TargetAbort/etc).
// `tag` appears in the log line so we can correlate snapshots taken at different points
// (PrepareHardware end, IOCTL_INFER pre-submit, IOCTL_INFER post-completion, post-timeout).
VOID npudriverDumpPciCommand(WDFDEVICE Device, const char* tag);

// PCIe Express Capability + AER (Advanced Error Reporting) dump.
//   PCIe Cap (at config 0x80): Device Status bits = correctable / non-fatal / fatal / unsup-req error
//   AER (extended config 0x100+): Uncorrectable + Correctable error status registers
// Goal: confirm whether the chip actually emits outbound TLPs onto PCIe link.
//   AER counters incrementing → chip emits TLPs, root complex sees them
//   AER counters stay 0 → chip's AXI activity never reaches PCIe link (silicon issue)
VOID npudriverDumpPciAer(WDFDEVICE Device, const char* tag);
