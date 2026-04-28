#include "Driver.h"
#include "Memory.h"
#include "Queue.h"
#include <wdmguid.h>   // GUID_BUS_INTERFACE_STANDARD

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, npudriverCreateDevice)
#pragma alloc_text(PAGE, npudriverEvtDevicePrepareHardware)
#pragma alloc_text(PAGE, npudriverEvtDeviceReleaseHardware)
#endif

NTSTATUS npudriverSettingResourceInfo(WDFDEVICE Device, WDFCMRESLIST ResourceList);
VOID npudriverReadTemperature(WDFDEVICE Device);
NTSTATUS npudriverDumpMsixCapability(WDFDEVICE Device);

NTSTATUS npudriverCreateDevice(PWDFDEVICE_INIT DeviceInit)
{
	WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
	WDF_FILEOBJECT_CONFIG fileObjectConfig;
	WDF_OBJECT_ATTRIBUTES deviceAttributes;
	PDEVICE_CONTEXT deviceContext;
	WDFDEVICE device;
	NTSTATUS status;

	PAGED_CODE();

	WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
	pnpPowerCallbacks.EvtDevicePrepareHardware = npudriverEvtDevicePrepareHardware;
	pnpPowerCallbacks.EvtDeviceReleaseHardware= npudriverEvtDeviceReleaseHardware;

	WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

	// Set up file object config with cleanup callback
	WDF_FILEOBJECT_CONFIG_INIT(&fileObjectConfig, NULL, NULL, npudriverEvtFileCleanup);
	WdfDeviceInitSetFileObjectConfig(DeviceInit, &fileObjectConfig, WDF_NO_OBJECT_ATTRIBUTES);

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);

	status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);

	if (NT_SUCCESS(status))
	{
		deviceContext = DeviceGetContext(device);

		// Initialize the context
		deviceContext->PrivateDeviceData = 0;
		deviceContext->LockedModelMdl = NULL;
		deviceContext->LockedModelSize = 0;
		deviceContext->DeviceStatus = DEVICE_STATUS_DEAD;
		deviceContext->Bar0BaseAddress = NULL;
		deviceContext->Bar0Length = 0;

		// Initialize inference MDLs
		deviceContext->InferInputMdl = NULL;
		deviceContext->InferOutputMdl = NULL;

		// Cached parameter MDL (IOCTL_PARAM_CACHE 가 채움)
		deviceContext->CachedParamMdl = NULL;
		deviceContext->CachedParamPteIdx = 0;
		deviceContext->CachedParamPageCount = 0;

		// Initialize descriptor ring fields
		deviceContext->DescRingBase = NULL;
		deviceContext->DescRingDeviceVA = 0;
		deviceContext->DescRingTail = 0;
		deviceContext->StatusBlockBase = NULL;
		deviceContext->StatusBlockDeviceVA = 0;

		// Initialize inference completion event
		KeInitializeEvent(&deviceContext->InferCompleteEvent, NotificationEvent, FALSE);

		// Create interrupt handlers for all 4 MSI-X vectors that chip exposes.
		// Same ISR/DPC for all — MessageID parameter in ISR distinguishes which
		// vector fired. Without registering all 4, Windows may route chip's
		// interrupt to a vector with no handler → ISR never fires.
		{
			deviceContext->IsrCallCount = 0;
			for (ULONG i = 0; i < 4; ++i) {
				WDF_INTERRUPT_CONFIG interruptConfig;
				WDF_INTERRUPT_CONFIG_INIT(&interruptConfig,
					npudriverEvtInterruptIsr,
					npudriverEvtInterruptDpc);
				interruptConfig.EvtInterruptEnable  = npudriverEvtInterruptEnable;
				interruptConfig.EvtInterruptDisable = npudriverEvtInterruptDisable;
				status = WdfInterruptCreate(device, &interruptConfig,
					WDF_NO_OBJECT_ATTRIBUTES, &deviceContext->Interrupts[i]);
				if (!NT_SUCCESS(status)) {
					DbgPrint("[%s] WdfInterruptCreate[%lu] failed: 0x%x\n",
						__FUNCTION__, i, status);
					return status;
				}
				DbgPrint("[%s] WdfInterruptCreate[%lu] OK (Interrupts[%lu]=%p)\n",
					__FUNCTION__, i, i, deviceContext->Interrupts[i]);
			}
		}

		// Create a device interface so that application can find and talk to us

		status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_npudriver, NULL);

		if (NT_SUCCESS(status))
		{
			// Initialize the I/O Queue
			WDF_IO_QUEUE_CONFIG queueConfig;
			WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchSequential);
			queueConfig.EvtIoDeviceControl = npudriverEvtIoDeviceControl;

			status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &deviceContext->IoQueue);
			if (!NT_SUCCESS(status)) {
				DbgPrint("[%s] WdfIoQueueCreate failed: 0x%x\n", __FUNCTION__, status);
				return status;
			}
		}
	}

	return status;
}

NTSTATUS
npudriverEvtDevicePrepareHardware(
	WDFDEVICE Device,
	WDFCMRESLIST ResourceList,
	WDFCMRESLIST ResourceListTranslated
)
{
	UNREFERENCED_PARAMETER(Device);
	UNREFERENCED_PARAMETER(ResourceListTranslated);

	NTSTATUS status = STATUS_SUCCESS;

	DbgPrint("[%s] Entry\n", __FUNCTION__);

	npudriverSettingResourceInfo(Device, ResourceList);
	npudriverReadTemperature(Device);

	PDEVICE_CONTEXT deviceContext = DeviceGetContext(Device);

	// =============================================================
	// SAVE MSI-X table BEFORE any chip reset.
	//
	// Reason: GCB reset (RAM shutdown -> RAM enable) wipes chip's
	// internal SRAM that backs BAR2+0x46800. Windows wrote MSI host
	// addr/data into this region during PCI enumeration. If we don't
	// restore them after reset, chip cannot DMA MSI TLPs to host APIC
	// -> no interrupts ever fire. Linux gasket handles this via
	// gasket_interrupt_reinit_msix(). Windows KMDF does NOT auto-replay
	// MSI-X programming after our chip-private reset, so we mirror it.
	// =============================================================
	if (deviceContext->Bar2BaseAddress) {
		ULONG i;
		DbgPrint("[%s] MSI-X SAVE (pre-reset) — reading BAR2+0x%x for 4 vectors\n",
			__FUNCTION__, APEX_REG_KERNEL_HIB_MSIX_TABLE);
		for (i = 0; i < APEX_INTERRUPT_COUNT; i++) {
			ULONG ent = APEX_REG_KERNEL_HIB_MSIX_TABLE + i * APEX_MSIX_VECTOR_SIZE;
			deviceContext->SavedMsixTable[i*4 + 0] = apex_read_register_32(deviceContext->Bar2BaseAddress, ent + 0);
			deviceContext->SavedMsixTable[i*4 + 1] = apex_read_register_32(deviceContext->Bar2BaseAddress, ent + 4);
			deviceContext->SavedMsixTable[i*4 + 2] = apex_read_register_32(deviceContext->Bar2BaseAddress, ent + 8);
			deviceContext->SavedMsixTable[i*4 + 3] = apex_read_register_32(deviceContext->Bar2BaseAddress, ent + 12);
			DbgPrint("[MSIX-SAVE] vec[%lu] addr=0x%08x_%08x data=0x%08x ctrl=0x%x\n",
				i,
				deviceContext->SavedMsixTable[i*4 + 1],
				deviceContext->SavedMsixTable[i*4 + 0],
				deviceContext->SavedMsixTable[i*4 + 2],
				deviceContext->SavedMsixTable[i*4 + 3]);
		}
		deviceContext->MsixTableSaved = TRUE;
	}

	// Initialize page table for memory mapping
	status = ApexPageTableInit(Device);
	if (!NT_SUCCESS(status)) {
		DbgPrint("[%s] ApexPageTableInit failed: 0x%x\n", __FUNCTION__, status);
		return status;
	}

	// =================================================================
	// SCU_CTRL_0: clear inactive PHY mode bits.
	//   bits[10:8]  rg_pcie_inact_phy_mode (3 bits)  -> 0
	//   bits[13:11] rg_usb_inact_phy_mode  (3 bits)  -> 0
	// libedgetpu BeagleTopLevelHandler::Open() does this as the very first
	// chip access. Without it, PCIe/USB PHY can stay in dormant mode and the
	// chip's SCU silently rejects subsequent CSR writes (notably RUN_CONTROL).
	// This is the missing step that explains why RUN_STATUS stays at 0
	// across all engines after our PrepareHardware writes.
	// =================================================================
	{
		UINT32 scu0Before = apex_read_register_32(deviceContext->Bar2BaseAddress,
			APEX_REG_SCU_CTRL_0);
		// Clear bits 8..13 (mask = 0x3F00 in lower 16 bits)
		UINT32 scu0After = scu0Before & ~((UINT32)0x3F00);
		apex_write_register_32(deviceContext->Bar2BaseAddress, APEX_REG_SCU_CTRL_0, scu0After);
		UINT32 scu0Verify = apex_read_register_32(deviceContext->Bar2BaseAddress,
			APEX_REG_SCU_CTRL_0);
		DbgPrint("[%s] SCU_CTRL_0 PHY-mode clear: before=0x%08x after-write=0x%08x readback=0x%08x\n",
			__FUNCTION__, scu0Before, scu0After, scu0Verify);
	}

	// Pre-reset: GCB 리셋 전에 반드시 수행해야 하는 HIB 엔진 초기화.
	// 공식 coral.sys 분석 결과 이 시퀀스 없이는 처리 클럭 도메인이 게이팅된 채 유지되어
	// TILE_CONFIG0 쓰기가 적용되지 않고 모든 RUN_STATUS가 0에서 변하지 않음.
	DbgPrint("[%s] Pre-reset: IDLEGENERATOR bit32 클리어\n", __FUNCTION__);
	{
		UINT64 idlegenVal = apex_read_register(deviceContext->Bar2BaseAddress, APEX_REG_IDLEGENERATOR);
		idlegenVal &= 0xFFFFFFFEFFFFFFFFULL;
		apex_write_register(deviceContext->Bar2BaseAddress, APEX_REG_IDLEGENERATOR, idlegenVal);
		KeMemoryBarrier();
	}

	DbgPrint("[%s] Pre-reset: DMA pause 요청 (0x486D8 = 1)\n", __FUNCTION__);
	apex_write_register(deviceContext->Bar2BaseAddress, APEX_REG_USER_HIB_DMA_PAUSE, 1);
	KeMemoryBarrier();

	// gasket driver 조건: (value & 1) == 1 — 이전 코드는 value == 2 로 틀렸음
	DbgPrint("[%s] Pre-reset: DMA pause 완료 대기 (0x486E0 bit0 == 1)\n", __FUNCTION__);
	{
		int retry;
		for (retry = 0; retry < 120; retry++) {
			UINT64 pausedStatus = apex_read_register(deviceContext->Bar2BaseAddress, APEX_REG_USER_HIB_DMA_PAUSED);
			if ((pausedStatus & 1) == 1) {
				DbgPrint("[%s] DMA pause 확인 (0x486E0=0x%llx, %d회 시도)\n", __FUNCTION__, pausedStatus, retry);
				break;
			}
			KeStallExecutionProcessor(100);
		}
		if (retry >= 120) {
			DbgPrint("[%s] WARNING: DMA pause 타임아웃 (DMA 비활성 상태였을 수 있음, 계속 진행)\n", __FUNCTION__);
		}
	}

	// Reset and quit-reset sequence to enable GCB (Global Clock Block)
	// This is required for the scalar core to be operational
	DbgPrint("[%s] Starting GCB reset sequence\n", __FUNCTION__);
	apex_rmw_register_32(deviceContext->Bar2BaseAddress, APEX_REG_SCU_2, 0x1, 2, 2);  // Enable GCB reset
	apex_rmw_register_32(deviceContext->Bar2BaseAddress, APEX_REG_SCU_2, 0x1, 2, 18); // Enable clock gate
	apex_rmw_register_32(deviceContext->Bar2BaseAddress, APEX_REG_SCU_3, 0x3, 2, 14); // Force RAM shutdown

	// Wait for RAM shutdown to complete (bit 6 of SCU_3 should be set)
	{
		UINT32 scu3_val = 0;
		int retry;
		for (retry = 0; retry < 100; retry++) {
			scu3_val = apex_read_register_32(deviceContext->Bar2BaseAddress, APEX_REG_SCU_3);
			if ((scu3_val & (1 << 6)) != 0) {
				DbgPrint("[%s] RAM shutdown confirmed (SCU_3=0x%08x)\n", __FUNCTION__, scu3_val);
				break;
			}
			KeStallExecutionProcessor(100); // 100 microseconds
		}
		if (retry >= 100) {
			DbgPrint("[%s] WARNING: RAM shutdown timeout after 10ms (SCU_3=0x%08x)\n", __FUNCTION__, scu3_val);
		}
	}

	// EnableReset step 5 (libedgetpu beagle_top_level_handler.cc:221-223):
	// Clear BULK credit by pulsing LSBs of gcbb_credit0 register.
	// Without this pulse, the AXI bridge between host and GCB keeps stale BULK credits
	// from the previous device session — scalar core's first DMA push to INFEED then
	// hangs internally, and INFEED auto-halts to kHalted=0x4. This is the prime suspect
	// for "INFEED stuck at kHalted after PARAM_CACHE" symptom we've been chasing.
	DbgPrint("[%s] Pulsing gcbb_credit0 to clear stale BULK credit\n", __FUNCTION__);
	apex_write_register_32(deviceContext->Bar2BaseAddress, APEX_REG_GCBB_CREDIT0, 0xF);
	apex_write_register_32(deviceContext->Bar2BaseAddress, APEX_REG_GCBB_CREDIT0, 0x0);

	DbgPrint("[%s] Starting GCB quit-reset sequence\n", __FUNCTION__);
	apex_rmw_register_32(deviceContext->Bar2BaseAddress, APEX_REG_SCU_3, 0x0, 2, 14); // Disable RAM shutdown
	// rg_gated_gcb (SCU_2 bits[19:18]) — libedgetpu values:
	//   0x0 = deprecated, 0x1 = hardware clock gated, 0x2 = no clock gating (force on)
	// 0x0 (deprecated) puts GCB in undefined gating state — large bitstreams (INFER)
	// stall mid-execution. 0x2 (force on) matches DisableHardwareClockGate().
	apex_rmw_register_32(deviceContext->Bar2BaseAddress, APEX_REG_SCU_2, 0x2, 2, 18); // rg_gated_gcb = no clock gating
	apex_rmw_register_32(deviceContext->Bar2BaseAddress, APEX_REG_SCU_2, 0x2, 2, 2);  // Exit reset

	// Wait for RAM enable to complete (bit 6 of SCU_3 should clear)
	{
		UINT32 scu3_val = 0;
		int retry;
		for (retry = 0; retry < 100; retry++) {
			scu3_val = apex_read_register_32(deviceContext->Bar2BaseAddress, APEX_REG_SCU_3);
			if ((scu3_val & (1 << 6)) == 0) {
				DbgPrint("[%s] RAM enable confirmed (SCU_3=0x%08x)\n", __FUNCTION__, scu3_val);
				break;
			}
			KeStallExecutionProcessor(100); // 100 microseconds
		}
		if (retry >= 100) {
			DbgPrint("[%s] WARNING: RAM enable timeout after 10ms (SCU_3=0x%08x)\n", __FUNCTION__, scu3_val);
		}
	}

	// Critical: Confirm reset is completely released by polling SCALAR_RUN_CONTROL
	// This register should read as 0 after reset is released, confirming the chip
	// has exited reset state and CSR accesses are valid.
	{
		int retry;
		for (retry = 0; retry < 100; retry++) {
			UINT64 scRunCtrl = apex_read_register(deviceContext->Bar2BaseAddress, APEX_REG_SCALAR_RUN_CONTROL);
			if (scRunCtrl == 0) {
				DbgPrint("[%s] Reset confirmed - SCALAR_RUN_CONTROL = 0x0\n", __FUNCTION__);
				break;
			}
			KeStallExecutionProcessor(100); // 100 microseconds
		}
		if (retry >= 100) {
			DbgPrint("[%s] WARNING: Reset confirmation timeout - SCALAR_RUN_CONTROL != 0\n", __FUNCTION__);
		}
	}

	// rg_pwr_state_ovr SCU_3[27:26] — 0x3 = all low-power modes disabled (max active).
	// libedgetpu uses 0x2 if hw_clock_gating allowed, 0x3 if not. We set 0x3 so chip
	// can never drop into Inactive or Sleep mode regardless of external triggers —
	// keeps RUN_CONTROL writes effective and AXI bus alive.
	apex_rmw_register_32(deviceContext->Bar2BaseAddress, APEX_REG_SCU_3, 0x3, 2, 26);
	{
		UINT32 scu3After = apex_read_register_32(deviceContext->Bar2BaseAddress, APEX_REG_SCU_3);
		DbgPrint("[%s] SCU_3 after power-state override (0x3=all-modes-off): 0x%08x\n",
			__FUNCTION__, scu3After);
	}

	// Clear AXI quiesce — libedgetpu's two-step DisableSoftwareClockGate +
	// DisableHardwareClockGate sequence (mmio_driver.cc DoOpen lines 4 & 5).
	//
	// Step A (DisableSoftwareClockGate):
	//   SCU_2 bits[19:18] = 0  (rg_gated_gcb = no gating, normal mode)
	//   AXI_QUIESCE bit 16   = 0
	// Step B (DisableHardwareClockGate, after Step A):
	//   SCU_2 bits[19:18] = 2  (rg_gated_gcb = force clock on, override gating)
	//
	// Going straight to bits=2 (as we did before) skips the "no gating" intermediate
	// state and the chip's AXI logic doesn't release bit 21 (axi_quiesced status).
	{
		PVOID bar2 = deviceContext->Bar2BaseAddress;
		UINT32 aq0 = apex_read_register_32(bar2, APEX_REG_AXI_QUIESCE);
		DbgPrint("[%s] AXI_QUIESCE before clear: 0x%08x (bit21=%u bit16=%u)\n",
			__FUNCTION__, aq0, (aq0 >> 21) & 1, (aq0 >> 16) & 1);

		// Step A1: SCU_2 bits[19:18] = 0 (DisableSoftwareClockGate)
		apex_rmw_register_32(bar2, APEX_REG_SCU_2, 0x0, 2, 18);
		KeStallExecutionProcessor(100);

		// Step A2: AXI_QUIESCE bit 16 = 0 (clear quiesce request)
		apex_rmw_register_32(bar2, APEX_REG_AXI_QUIESCE, 0x0, 1, 16);
		KeStallExecutionProcessor(100);

		UINT32 aqA = apex_read_register_32(bar2, APEX_REG_AXI_QUIESCE);
		DbgPrint("[%s] AXI_QUIESCE after Step A (gate=0, bit16=0): 0x%08x (bit21=%u)\n",
			__FUNCTION__, aqA, (aqA >> 21) & 1);

		// Step B: SCU_2 bits[19:18] = 2 (DisableHardwareClockGate, force on)
		apex_rmw_register_32(bar2, APEX_REG_SCU_2, 0x2, 2, 18);
		KeStallExecutionProcessor(100);

		// Poll bit 21 to clear (axi_quiesced status follows axi_quiesce_request after
		// clock gating is fully disabled).
		{
			int t;
			for (t = 0; t < 1000; t++) {
				UINT32 aq = apex_read_register_32(bar2, APEX_REG_AXI_QUIESCE);
				if ((aq & (1u << 21)) == 0) {
					DbgPrint("[%s] AXI_QUIESCE bit 21 cleared after %d polls (val=0x%08x)\n",
						__FUNCTION__, t, aq);
					break;
				}
				KeStallExecutionProcessor(10);
			}
		}

		UINT32 aqB = apex_read_register_32(bar2, APEX_REG_AXI_QUIESCE);
		DbgPrint("[%s] AXI_QUIESCE after Step B (gate=2, force on): 0x%08x (bit21=%u)\n",
			__FUNCTION__, aqB, (aqB >> 21) & 1);
	}

	// Unpause DMA engines — the pause written before reset must be explicitly cleared.
	// GCB reset may or may not clear this register; write 0 unconditionally.
	apex_write_register(deviceContext->Bar2BaseAddress, APEX_REG_USER_HIB_DMA_PAUSE, 0);
	DbgPrint("[%s] USER_HIB_DMA_PAUSE cleared (DMA unpaused)\n", __FUNCTION__);

	DbgPrint("[%s] GCB reset/quit-reset sequence complete\n", __FUNCTION__);

	// ExitReset step 4 (libedgetpu beagle_top_level_handler.cc::QuitReset):
	// IdleRegister: bit[31]=disable_idle (0=on, 1=off), bits[30:0]=counter.
	// We DISABLE auto-idle (set bit31=1) so the chip does NOT power-gate INFEED/
	// PARAM_POP after PARAM_CACHE bitstream completes. With idle enabled (0x1),
	// engines drop to kHalted=4 during the IOCTL gap and refuse RUN_CONTROL
	// writes — INFER then deadlocks because SCALAR can't dispatch to halted
	// INFEED. libedgetpu submits PARAM_CACHE+INFER back-to-back so the gap is
	// too short to trigger auto-idle, but our two-IOCTL flow has a real gap.
	{
		PVOID bar2 = deviceContext->Bar2BaseAddress;
		apex_write_register(bar2, APEX_REG_IDLEGENERATOR, 0x80000001ULL);
		DbgPrint("[%s] IdleRegister → 0x80000001 (disable_idle=1, counter=1)\n", __FUNCTION__);

		// Re-write HIB MMU config after GCB reset — these registers may have been
		// cleared by the reset even though they were written in ApexPageTableInit().
		apex_write_register(bar2, APEX_REG_PAGE_TABLE_SIZE, (UINT64)APEX_PAGE_TABLE_ENTRIES);
		apex_write_register(bar2, APEX_REG_EXTENDED_TABLE, 6144);
		DbgPrint("[%s] PAGE_TABLE_SIZE=%u EXTENDED_TABLE=6144 re-written after GCB reset\n",
			__FUNCTION__, APEX_PAGE_TABLE_ENTRIES);

		// TILE_CONFIG0 and TILE_DEEP_SLEEP are written in IOCTL_INFER after bitstream
		// is mapped, so tiles don't access unmapped VAs during device open.
	}

	// Initialize interrupt routing
	// Route SC_HOST_0 completion interrupt to MSI-X vector 0 (our WdfInterrupt)
	if (deviceContext->Bar2BaseAddress != NULL) {
		// SC_HOST_INTVECCTL: bits[3:0] = vector number (0 = MSI-X message 0)
		apex_write_register(deviceContext->Bar2BaseAddress,
						   APEX_REG_SC_HOST_INTVECCTL, 0);
		DbgPrint("[%s] SC_HOST_INTVECCTL set to route to MSI-X vector 0\n", __FUNCTION__);

		// SC_HOST_INT_CONTROL: enable ALL 4 SC_HOST completion interrupts (kNumInterrupts=4
		// per scalar_core_controller.cc:32; libedgetpu writes (1<<4)-1=0xF, NOT 1).
		apex_write_register(deviceContext->Bar2BaseAddress,
						   APEX_REG_SC_HOST_INT_CONTROL, 0xF);
		DbgPrint("[%s] SC_HOST_INT_CONTROL → 0xF (4 SC_HOST interrupts enabled)\n", __FUNCTION__);

		// TOP_LEVEL_INT_CONTROL: enable top-level aggregator (libedgetpu writes 0xF).
		apex_write_register(deviceContext->Bar2BaseAddress,
						   APEX_REG_TOP_LEVEL_INT_CONTROL, 0xF);
		DbgPrint("[%s] TOP_LEVEL_INT_CONTROL → 0xF\n", __FUNCTION__);

		// FATAL_ERR_INT_CONTROL: enable fatal-error interrupt so HIB errors notify host.
		apex_write_register(deviceContext->Bar2BaseAddress,
						   APEX_REG_FATAL_ERR_INT_CONTROL, 1);
		DbgPrint("[%s] FATAL_ERR_INT_CONTROL → 1\n", __FUNCTION__);

		// DMA_BURST_LIMITER: libedgetpu explicitly writes 0 here at chip open
		// (mmio_driver.cc:288-295). Default is unspecified; chip may stall on long bursts.
		apex_write_register(deviceContext->Bar2BaseAddress,
						   APEX_REG_DMA_BURST_LIMITER, 0);
		DbgPrint("[%s] DMA_BURST_LIMITER → 0 (explicit)\n", __FUNCTION__);

		// INSTR_QUEUE_INTVECCTL: route instruction queue interrupt to MSI-X vector 0
		apex_write_register(deviceContext->Bar2BaseAddress,
						   APEX_REG_INSTR_QUEUE_INTVECCTL, 0);
		DbgPrint("[%s] INSTR_QUEUE_INTVECCTL → MSI-X vector 0\n", __FUNCTION__);

		// Other queue/error INTVECCTL — gasket routes each to a unique vector but
		// since we register only 4 vectors and all collapse to same handler, route to 0.
		apex_write_register(deviceContext->Bar2BaseAddress,
						   APEX_REG_INPUT_ACTV_QUEUE_INTVECCTL, 0);
		apex_write_register(deviceContext->Bar2BaseAddress,
						   APEX_REG_PARAM_QUEUE_INTVECCTL, 0);
		apex_write_register(deviceContext->Bar2BaseAddress,
						   APEX_REG_OUTPUT_ACTV_QUEUE_INTVECCTL, 0);
		apex_write_register(deviceContext->Bar2BaseAddress,
						   APEX_REG_FATAL_ERR_INTVECCTL, 0);
		DbgPrint("[%s] INPUT/PARAM/OUTPUT_ACTV/FATAL_ERR INTVECCTL → vector 0\n", __FUNCTION__);

		// INSTR_QUEUE_INT_CONTROL: enable IQ completion interrupt (host_queue.h:84-85).
		// Without this, IQ_INT_STATUS stays 0 and WIRE_INT_PENDING never fires for IQ.
		apex_write_register(deviceContext->Bar2BaseAddress,
						   APEX_REG_INSTR_QUEUE_INT_CONTROL, 1);
		DbgPrint("[%s] INSTR_QUEUE_INT_CONTROL → 1 (IQ interrupt enabled)\n", __FUNCTION__);

		// TOP_LEVEL_INTVECCTL: routes the aggregated WIRE_INT_PENDING signal to
		// MSI-X vector 0. Without this, WIRE_INT_PENDING is set but no MSI-X
		// write transaction is generated → ISR never called.
		apex_write_register(deviceContext->Bar2BaseAddress,
						   APEX_REG_TOP_LEVEL_INTVECCTL, 0);
		DbgPrint("[%s] TOP_LEVEL_INTVECCTL → MSI-X vector 0\n", __FUNCTION__);

		// WIRE_INT_MASK: unmask all wire interrupts (value 0 = all unmasked)
		apex_write_register(deviceContext->Bar2BaseAddress,
						   APEX_REG_WIRE_INT_MASK, 0);
		DbgPrint("[%s] WIRE_INT_MASK cleared (interrupts unmasked)\n", __FUNCTION__);

		// =====================================================================
		// MSI-X TABLE UNMASK — chip-private MSI-X table at BAR2+0x46800.
		// Read PCI MSI-X capability directly from PCI config space to discover
		// the *real* table BIR/Offset that Windows uses. If chip's BAR2+0x46800
		// table stays zero, it's because Windows pointed MSI-X somewhere else.
		npudriverDumpMsixCapability(Device);

		// =============================================================
		// RESTORE MSI-X table (pair to the SAVE done before GCB reset).
		// Without this, chip's BAR2+0x46800 RAM is full of zeros and the
		// chip's MSI engine has no idea where to DMA the TLP to.
		// =============================================================
		if (deviceContext->MsixTableSaved) {
			ULONG i;
			DbgPrint("[%s] MSI-X RESTORE (post-reset) — writing 4 saved entries\n",
				__FUNCTION__);
			for (i = 0; i < APEX_INTERRUPT_COUNT; i++) {
				ULONG ent = APEX_REG_KERNEL_HIB_MSIX_TABLE + i * APEX_MSIX_VECTOR_SIZE;
				apex_write_register_32(deviceContext->Bar2BaseAddress, ent + 0,
					deviceContext->SavedMsixTable[i*4 + 0]);
				apex_write_register_32(deviceContext->Bar2BaseAddress, ent + 4,
					deviceContext->SavedMsixTable[i*4 + 1]);
				apex_write_register_32(deviceContext->Bar2BaseAddress, ent + 8,
					deviceContext->SavedMsixTable[i*4 + 2]);
				// vector_ctrl restored without the mask bit — the explicit
				// mask-clear below also forces this to 0.
				apex_write_register_32(deviceContext->Bar2BaseAddress, ent + 12, 0);
				DbgPrint("[MSIX-REST] vec[%lu] wrote addr=0x%08x_%08x data=0x%08x\n",
					i,
					deviceContext->SavedMsixTable[i*4 + 1],
					deviceContext->SavedMsixTable[i*4 + 0],
					deviceContext->SavedMsixTable[i*4 + 2]);
			}
		}

		// Apex chip places its MSI-X table inside BAR2 (NOT in standard PCI cap space).
		// Linux gasket explicitly clears mask bits because pci_enable_msix_exact does
		// not touch chip-private tables (gasket_interrupt.c::force_msix_interrupt_unmasking).
		// Windows KMDF likewise does NOT touch this region — without this, the chip
		// silently drops every MSI-X TLP and our ISR is never called.
		// Layout: 16 bytes per vector, mask bit at byte offset +12 (MSIX_MASK_BIT_OFFSET).
		// =====================================================================
		{
			ULONG i;
			for (i = 0; i < APEX_INTERRUPT_COUNT; i++) {
				ULONG mask_off = APEX_REG_KERNEL_HIB_MSIX_TABLE
				                 + APEX_MSIX_MASK_BIT_OFFSET
				                 + i * APEX_MSIX_VECTOR_SIZE;
				apex_write_register_32(deviceContext->Bar2BaseAddress, mask_off, 0);
			}
			DbgPrint("[%s] MSI-X table mask bits cleared for %u vectors (BAR2+0x4680C..)\n",
				__FUNCTION__, APEX_INTERRUPT_COUNT);

			// FULL MSI-X table dump — entry layout (16 bytes each):
			//   +0:  msg_addr_lo (host MSI address low)
			//   +4:  msg_addr_hi (host MSI address high)
			//   +8:  msg_data    (data Windows wants written by chip)
			//   +12: vector_ctrl (bit 0 = mask)
			// If addr/data are 0, Windows did NOT discover this MSI-X table —
			// chip can't send TLP because it doesn't know where to send.
			for (i = 0; i < APEX_INTERRUPT_COUNT; i++) {
				ULONG ent = APEX_REG_KERNEL_HIB_MSIX_TABLE + i * APEX_MSIX_VECTOR_SIZE;
				UINT32 alo = apex_read_register_32(deviceContext->Bar2BaseAddress, ent + 0);
				UINT32 ahi = apex_read_register_32(deviceContext->Bar2BaseAddress, ent + 4);
				UINT32 data= apex_read_register_32(deviceContext->Bar2BaseAddress, ent + 8);
				UINT32 mb  = apex_read_register_32(deviceContext->Bar2BaseAddress, ent + 12);
				DbgPrint("[MSIX] vector[%u] addr=0x%08x_%08x data=0x%08x mask=0x%x\n",
					i, ahi, alo, data, mb);
			}
		}

		// gasket driver: poll both PAGE_TABLE_INIT and MSIX_TABLE_INIT together.
		// Both must be non-zero before the HIB is considered fully initialized.
		{
			int retry;
			for (retry = 0; retry < 120; retry++) {
				UINT64 ptInit   = apex_read_register(deviceContext->Bar2BaseAddress, APEX_REG_KERNEL_HIB_PAGE_TABLE_INIT);
				UINT64 msixInit = apex_read_register(deviceContext->Bar2BaseAddress, APEX_REG_MSIX_TABLE_INIT);
				if (ptInit != 0 && msixInit != 0) {
					DbgPrint("[%s] HIB 초기화 완료: PAGE_TABLE_INIT=0x%llx MSIX_TABLE_INIT=0x%llx (%d회)\n",
						__FUNCTION__, ptInit, msixInit, retry);
					break;
				}
				DbgPrint("[%s] HIB 대기 중 (retry=%d): PT=0x%llx MSIX=0x%llx\n",
					__FUNCTION__, retry, ptInit, msixInit);
				KeStallExecutionProcessor(100);
			}
			if (retry >= 120) {
				DbgPrint("[%s] WARNING: HIB 초기화 타임아웃\n", __FUNCTION__);
			}
		}

		// Read error statuses at init to see baseline state
		UINT32 initHibError = apex_read_register_32(deviceContext->Bar2BaseAddress, APEX_REG_USER_HIB_ERROR_STATUS);
		UINT32 initScError = apex_read_register_32(deviceContext->Bar2BaseAddress, APEX_REG_SCALAR_CORE_ERROR_STATUS);
		DbgPrint("[%s] USER_HIB_ERROR at init = 0x%08x, SCALAR_CORE_ERROR = 0x%08x\n",
			__FUNCTION__, initHibError, initScError);
	}

	// Allocate descriptor ring (4KB = 256 slots * 16 bytes each)
	// Maps to PTE slot 6142 (last-2 simple entry), device VA = 6142 * 4KB = 0x17FE000
	#pragma warning(push)
	#pragma warning(disable:4996)
	deviceContext->DescRingBase = ExAllocatePoolWithTag(NonPagedPoolNx, PAGE_SIZE, 'DRNG');
	#pragma warning(pop)
	if (deviceContext->DescRingBase == NULL) {
		DbgPrint("[%s] Failed to allocate descriptor ring\n", __FUNCTION__);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	RtlZeroMemory(deviceContext->DescRingBase, PAGE_SIZE);
	{
		PHYSICAL_ADDRESS descPhys = MmGetPhysicalAddress(deviceContext->DescRingBase);
		deviceContext->DescRingDeviceVA = (UINT64)6142 * PAGE_SIZE;  // 0x17FE000
		deviceContext->DescRingTail = 0;
		apex_write_register(deviceContext->Bar2BaseAddress,
			APEX_REG_PAGE_TABLE + (6142 * 8), descPhys.QuadPart | 1);
		{
			UINT64 rb = apex_read_register(deviceContext->Bar2BaseAddress,
				APEX_REG_PAGE_TABLE + (6142 * 8));
			DbgPrint("[%s] DescRing: VA=%p PA=0x%llx DeviceVA=0x%llx PTE[6142] write=0x%llx readback=0x%llx %s\n",
				__FUNCTION__, deviceContext->DescRingBase, descPhys.QuadPart,
				deviceContext->DescRingDeviceVA, descPhys.QuadPart | 1, rb,
				(rb == (UINT64)(descPhys.QuadPart | 1)) ? "OK" : "MISMATCH");
		}
	}

	// Allocate status block (4KB) — hardware DMA-writes completion info here
	// Maps to PTE slot 6143 (last simple entry), device VA = 6143 * 4KB = 0x17FF000
	#pragma warning(push)
	#pragma warning(disable:4996)
	deviceContext->StatusBlockBase = ExAllocatePoolWithTag(NonPagedPoolNx, PAGE_SIZE, 'SBLK');
	#pragma warning(pop)
	if (deviceContext->StatusBlockBase == NULL) {
		DbgPrint("[%s] Failed to allocate status block\n", __FUNCTION__);
		ExFreePoolWithTag(deviceContext->DescRingBase, 'DRNG');
		deviceContext->DescRingBase = NULL;
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	RtlZeroMemory(deviceContext->StatusBlockBase, PAGE_SIZE);
	{
		PHYSICAL_ADDRESS sblkPhys = MmGetPhysicalAddress(deviceContext->StatusBlockBase);
		deviceContext->StatusBlockDeviceVA = (UINT64)6143 * PAGE_SIZE;  // 0x17FF000
		apex_write_register(deviceContext->Bar2BaseAddress,
			APEX_REG_PAGE_TABLE + (6143 * 8), sblkPhys.QuadPart | 1);
		{
			UINT64 rb = apex_read_register(deviceContext->Bar2BaseAddress,
				APEX_REG_PAGE_TABLE + (6143 * 8));
			DbgPrint("[%s] StatusBlock: VA=%p PA=0x%llx DeviceVA=0x%llx PTE[6143] write=0x%llx readback=0x%llx %s\n",
				__FUNCTION__, deviceContext->StatusBlockBase, sblkPhys.QuadPart,
				deviceContext->StatusBlockDeviceVA, sblkPhys.QuadPart | 1, rb,
				(rb == (UINT64)(sblkPhys.QuadPart | 1)) ? "OK" : "MISMATCH");
		}
	}

	// Signal page table initialization complete after writing PTE[8190/8191].
	apex_write_register(deviceContext->Bar2BaseAddress, APEX_REG_KERNEL_HIB_PAGE_TABLE_INIT, 1);
	DbgPrint("[%s] PAGE_TABLE_INIT signaled\n", __FUNCTION__);

	// Configure instruction queue CSRs (libedgetpu HostQueue::Open() step 5).
	// BASE/SIZE/STATUS_BLOCK must be written BEFORE enabling CTRL.
	apex_write_register(deviceContext->Bar2BaseAddress,
		APEX_REG_INSTR_QUEUE_BASE,         deviceContext->DescRingDeviceVA);
	apex_write_register(deviceContext->Bar2BaseAddress,
		APEX_REG_INSTR_QUEUE_STATUS_BLOCK, deviceContext->StatusBlockDeviceVA);
	apex_write_register(deviceContext->Bar2BaseAddress,
		APEX_REG_INSTR_QUEUE_SIZE,         256);
	apex_write_register(deviceContext->Bar2BaseAddress,
		APEX_REG_INSTR_QUEUE_DESC_SIZE,    16);  // sizeof(HOST_QUEUE_DESC) = 8+4+4
	apex_write_register(deviceContext->Bar2BaseAddress,
		APEX_REG_INSTR_QUEUE_TAIL,         0);
	deviceContext->DescRingTail = 0;  // sync sw counter with hw TAIL reset
	DbgPrint("[%s] Instruction queue configured: BASE=0x%llx STATUS_BLOCK=0x%llx SIZE=256 DESC_SIZE=16 TAIL=0\n",
		__FUNCTION__, deviceContext->DescRingDeviceVA, deviceContext->StatusBlockDeviceVA);

	// Verify both ring PTEs are valid before enabling queue
	{
		UINT64 rb6142 = apex_read_register(deviceContext->Bar2BaseAddress, APEX_REG_PAGE_TABLE + (6142 * 8));
		UINT64 rb6143 = apex_read_register(deviceContext->Bar2BaseAddress, APEX_REG_PAGE_TABLE + (6143 * 8));
		DbgPrint("[%s] PTE[6142](DescRing)=0x%llx PTE[6143](StatusBlock)=0x%llx\n",
			__FUNCTION__, rb6142, rb6143);
		if ((rb6142 & 1) == 0) DbgPrint("[%s] WARNING: PTE[6142] not valid!\n", __FUNCTION__);
		if ((rb6143 & 1) == 0) DbgPrint("[%s] WARNING: PTE[6143] not valid!\n", __FUNCTION__);
	}

	// Step A: enable only (bit0), no sb_wr_enable yet — isolate which bit causes fault
	apex_write_register(deviceContext->Bar2BaseAddress, APEX_REG_INSTR_QUEUE_CONTROL, 0x1);
	DbgPrint("[%s] INSTR_QUEUE_CONTROL → 0x1 (enable only)\n", __FUNCTION__);
	{
		UINT64 hibChk = apex_read_register(deviceContext->Bar2BaseAddress, APEX_REG_USER_HIB_ERROR_STATUS);
		DbgPrint("[%s] HIB_ERROR after CTRL=0x1 = 0x%llx\n", __FUNCTION__, hibChk);
	}

	// Clear any stale IQ interrupt pending from previous driver instance.
	// On driver reload the chip retains IQ_INT_STATUS=1 from the last run; if
	// we don't W1C-ack it before unmasking, the very first MSI-X delivery
	// after EvtInterruptEnable triggers an immediate ISR storm (no real
	// completion, just leftover state).
	{
		UINT64 staleIq = apex_read_register(deviceContext->Bar2BaseAddress,
			APEX_REG_INSTR_QUEUE_INT_STATUS);
		UINT64 stalePending = apex_read_register(deviceContext->Bar2BaseAddress,
			APEX_REG_WIRE_INT_PENDING);
		DbgPrint("[%s] stale IQ_INT_STATUS=0x%llx WIRE_INT_PENDING=0x%llx — clearing\n",
			__FUNCTION__, staleIq, stalePending);
		if (staleIq) {
			apex_write_register(deviceContext->Bar2BaseAddress,
				APEX_REG_INSTR_QUEUE_INT_STATUS, staleIq);  // W1C
		}
		if (stalePending) {
			apex_write_register(deviceContext->Bar2BaseAddress,
				APEX_REG_WIRE_INT_PENDING, stalePending);   // W1C
		}
	}

	// Step B: add sb_wr_enable (bit2)
	apex_write_register(deviceContext->Bar2BaseAddress, APEX_REG_INSTR_QUEUE_CONTROL, 0x5);
	DbgPrint("[%s] INSTR_QUEUE_CONTROL → 0x5 (enable + sb_wr_enable)\n", __FUNCTION__);
	{
		UINT64 hibChk = apex_read_register(deviceContext->Bar2BaseAddress, APEX_REG_USER_HIB_ERROR_STATUS);
		DbgPrint("[%s] HIB_ERROR after CTRL=0x5 = 0x%llx\n", __FUNCTION__, hibChk);
	}

	// Poll queue_status (0x48570) until bit0==1 (libedgetpu step 7).
	// Hardware must confirm enable before any TAIL writes are meaningful.
	{
		int retry;
		for (retry = 0; retry < 200; retry++) {
			UINT64 qs = apex_read_register(deviceContext->Bar2BaseAddress, APEX_REG_INSTR_QUEUE_STATUS);
			if (qs & 1) {
				DbgPrint("[%s] INSTR_QUEUE_STATUS = 0x%llx (enabled, %d tries)\n",
					__FUNCTION__, qs, retry);
				break;
			}
			KeStallExecutionProcessor(100);
		}
		if (retry >= 200) {
			UINT64 qs = apex_read_register(deviceContext->Bar2BaseAddress, APEX_REG_INSTR_QUEUE_STATUS);
			DbgPrint("[%s] WARNING: queue_status poll timeout STATUS=0x%llx\n", __FUNCTION__, qs);
		}
	}

	// Initialize all cores (breakpoints, tile config, run controls).
	// Per libedgetpu DoOpen(): run controls are set ONCE at device open time.
	// SCALAR enters kRunning but waits for a descriptor before executing anything,
	// so no bitstream access occurs here — no page fault risk.
	// INFEED/AVDATA stay in kRunning idle-loop until scalar pushes commands.
	// Per-inference run-control writes caused INFEED to halt before scalar could
	// push its first command, producing the INFEED=0x4/SCALAR=0x1 deadlock.
	{
		PVOID bar2 = deviceContext->Bar2BaseAddress;

		// DO NOT touch *_BREAKPOINT registers.
		//
		// libedgetpu (run_controller.cc, mmio_driver.cc) and gasket-driver
		// (apex_driver.c) never write to scalarCoreBreakPoint, infeedBreakPoint,
		// outfeedBreakPoint, opBreakPoint, etc. They leave the chip's POR default
		// in place — which is "breakpoint disabled".
		//
		// Our previous code wrote 0x7FFF intending to "set max PC" but the register
		// is 15-bit wide (readback always latched 0x7FFF), and 0x7FFF appears to be
		// interpreted by chip as a real "halt when PC reaches 0x7FFF" threshold.
		// That's why PARAM bitstream (9808 B, ~2452 instructions, PC stays low)
		// completed fine while INFER bitstream (199776 B, ~50K instructions, PC
		// crosses 0x7FFF) stalled forever — SCALAR hit the breakpoint mid-execution.
		//
		// Symptom that matches: COMPLETED stuck at 1 (PARAM done), SCALAR=kRun (1)
		// for a long time but never reaches the bitstream's completion instruction.
		DbgPrint("[%s] BREAKPOINT registers left at chip default (libedgetpu-style)\n",
			__FUNCTION__);

		// TILE_CONFIG0 broadcast to all 7 tile groups, poll for acknowledgment.
		// Must happen before tile run controls (run_controller.cc:118-128).
		apex_write_register(bar2, APEX_REG_TILE_CONFIG0, 0x7F);
		{
			int tci;
			for (tci = 0; tci < 1000; tci++) {
				if (apex_read_register(bar2, APEX_REG_TILE_CONFIG0) == 0x7F) break;
				KeStallExecutionProcessor(10);
			}
			DbgPrint("[%s] TILE_CONFIG0=0x7F confirmed after %d polls\n", __FUNCTION__, tci);
		}

		// TILE_DEEP_SLEEP: to_sleep_delay=2, to_wake_delay=30 (libedgetpu value).
		// Prevents SRAM access before wake stabilization (default 0x501 is too short).
		apex_write_register(bar2, APEX_REG_TILE_DEEP_SLEEP, 0x1E02ULL);

		// Run control sequence per libedgetpu run_controller.cc DoRunControl().
		// Phase 1: scalar-side units (SCALAR first).
		apex_write_register_32(bar2, APEX_REG_SCALAR_RUN_CONTROL,        1);
		apex_write_register_32(bar2, APEX_REG_AVDATA_POP_RUN_CONTROL,    1);
		apex_write_register_32(bar2, APEX_REG_PARAMETER_POP_RUN_CONTROL, 1);
		apex_write_register_32(bar2, APEX_REG_INFEED_RUN_CONTROL,        1);
		apex_write_register_32(bar2, APEX_REG_OUTFEED_RUN_CONTROL,       1);
		// Phase 2: tile-side units.
		apex_write_register_32(bar2, APEX_REG_TILE_OP_RUN_CONTROL,            1);
		apex_write_register_32(bar2, APEX_REG_NARROW_TO_WIDE_RUN_CONTROL,     1);
		apex_write_register_32(bar2, APEX_REG_WIDE_TO_NARROW_RUN_CONTROL,     1);
		apex_write_register_32(bar2, APEX_REG_MESH_BUS0_RUN_CONTROL,          1);
		apex_write_register_32(bar2, APEX_REG_MESH_BUS1_RUN_CONTROL,          1);
		apex_write_register_32(bar2, APEX_REG_MESH_BUS2_RUN_CONTROL,          1);
		apex_write_register_32(bar2, APEX_REG_MESH_BUS3_RUN_CONTROL,          1);
		apex_write_register_32(bar2, APEX_REG_RING_BUS_CONSUMER0_RUN_CONTROL, 1);
		apex_write_register_32(bar2, APEX_REG_RING_BUS_CONSUMER1_RUN_CONTROL, 1);
		apex_write_register_32(bar2, APEX_REG_RING_BUS_PRODUCER_RUN_CONTROL,  1);
		DbgPrint("[%s] Run controls set (all units kRunning)\n", __FUNCTION__);
	}

	// =====================================================================
	// READBACK VERIFICATION — 실제로 레지스터에 뭐가 써있는지 확인
	// =====================================================================
	{
		PVOID b = deviceContext->Bar2BaseAddress;

		// 큐 설정 확인
		DbgPrint("[RB] INSTR_QUEUE_CONTROL = 0x%llx  (기대 0x5)\n",
			apex_read_register(b, APEX_REG_INSTR_QUEUE_CONTROL));
		DbgPrint("[RB] INSTR_QUEUE_BASE    = 0x%llx  (기대 0x1ffe000)\n",
			apex_read_register(b, APEX_REG_INSTR_QUEUE_BASE));
		DbgPrint("[RB] INSTR_QUEUE_SIZE    = 0x%llx  (기대 0x100)\n",
			apex_read_register(b, APEX_REG_INSTR_QUEUE_SIZE));

		// 아이들/슬립 설정 확인
		DbgPrint("[RB] IDLEGENERATOR       = 0x%llx  (기대 0x80000001 disable_idle=1)\n",
			apex_read_register(b, APEX_REG_IDLEGENERATOR));
		DbgPrint("[RB] TILE_DEEP_SLEEP     = 0x%llx  (기대 0x1e02)\n",
			apex_read_register(b, APEX_REG_TILE_DEEP_SLEEP));

		// breakpoint 확인 — POR default 그대로 두는 중. INFER stall 이 사라지면
		// 이 값이 "disabled" 의미하는 chip default 임을 확인하는 데이터 포인트.
		DbgPrint("[RB] SCALAR_BREAKPOINT   = 0x%llx  (POR default, no longer overwritten)\n",
			apex_read_register(b, APEX_REG_SCALAR_BREAKPOINT));

		// 인터럽트 설정 확인
		DbgPrint("[RB] WIRE_INT_MASK       = 0x%llx  (기대 0x0)\n",
			apex_read_register(b, APEX_REG_WIRE_INT_MASK));
		DbgPrint("[RB] SC_HOST_INTVECCTL   = 0x%llx  (기대 0x0)\n",
			apex_read_register(b, APEX_REG_SC_HOST_INTVECCTL));

		// HIB 에러 — 32비트, 64비트 둘 다 읽어서 비교
		{
			UINT32 hibErr32 = apex_read_register_32(b, APEX_REG_USER_HIB_ERROR_STATUS);
			UINT64 hibErr64 = apex_read_register(b, APEX_REG_USER_HIB_ERROR_STATUS);
			DbgPrint("[RB] USER_HIB_ERROR(32)  = 0x%08x\n", hibErr32);
			DbgPrint("[RB] USER_HIB_ERROR(64)  = 0x%016llx\n", hibErr64);
			// 비트 디코딩 (HibError 클래스 기준)
			if (hibErr64 & (1ULL<<0))  DbgPrint("[RB]   bit0 : inbound_page_fault\n");
			if (hibErr64 & (1ULL<<1))  DbgPrint("[RB]   bit1 : extended_page_fault\n");
			if (hibErr64 & (1ULL<<2))  DbgPrint("[RB]   bit2 : csr_parity_error\n");
			if (hibErr64 & (1ULL<<3))  DbgPrint("[RB]   bit3 : axi_slave_b_error\n");
			if (hibErr64 & (1ULL<<4))  DbgPrint("[RB]   bit4 : axi_slave_r_error\n");
			if (hibErr64 & (1ULL<<5))  DbgPrint("[RB]   bit5 : instruction_queue_bad_configuration\n");
			if (hibErr64 & (1ULL<<6))  DbgPrint("[RB]   bit6 : input_actv_queue_bad_configuration\n");
			if (hibErr64 & (1ULL<<7))  DbgPrint("[RB]   bit7 : param_queue_bad_configuration\n");
			if (hibErr64 & (1ULL<<8))  DbgPrint("[RB]   bit8 : output_actv_queue_bad_configuration\n");
			if (hibErr64 & (1ULL<<9))  DbgPrint("[RB]   bit9 : instruction_queue_invalid\n");
			if (hibErr64 & (1ULL<<13)) DbgPrint("[RB]   bit13: length_0_dma\n");
			if (hibErr64 & (1ULL<<14)) DbgPrint("[RB]   bit14: virt_table_rdata_uncorr\n");

			/* Faulting VA — only valid when bit0 (inbound_page_fault) is set */
			if (hibErr64 & 1ULL) {
				UINT64 firstErr = apex_read_register(b, APEX_REG_USER_HIB_FIRST_ERROR);
				DbgPrint("[RB] HIB_FIRST_ERROR(0x48700) = 0x%016llx  (faulting device VA)\n", firstErr);
			}
		}

		// AXI quiesce — must be 0 for scalar→DMA internal CSR writes to work
		DbgPrint("[RB] AXI_QUIESCE         = 0x%08x  (기대 0x0)\n",
			apex_read_register_32(b, APEX_REG_AXI_QUIESCE));

		// DMA pause — must be 0 for DMA to operate after reset
		DbgPrint("[RB] USER_HIB_DMA_PAUSE  = 0x%llx  (기대 0x0)\n",
			apex_read_register(b, APEX_REG_USER_HIB_DMA_PAUSE));

		// SCU 상태 (cur_pwr_state bits[9:8] == 0이어야 active)
		{
			UINT32 scu3 = apex_read_register_32(b, APEX_REG_SCU_3);
			DbgPrint("[RB] SCU_3               = 0x%08x  (cur_pwr_state bits[9:8] = %u)\n",
				scu3, (scu3 >> 8) & 0x3);
		}
	}

	// BAR0 dump — find where Windows actually wrote MSI address/data.
	// Standard PCI MSI-X capability points to a BAR (often a separate one). For this
	// chip, BAR0 (length 0x4000) is the suspected location. If addr/data here are
	// non-zero we mirror them into the chip-private BAR2+0x46800 table.
	if (deviceContext->Bar0BaseAddress != NULL) {
		ULONG i;
		DbgPrint("[BAR0] dumping first 32 dwords of BAR0 (len=0x%x):\n",
			deviceContext->Bar0Length);
		for (i = 0; i < 32; i++) {
			UINT32 v = *(volatile UINT32*)((PUCHAR)deviceContext->Bar0BaseAddress + i*4);
			if ((i % 4) == 0) DbgPrint("[BAR0+0x%03x] ", i*4);
			DbgPrint("%08x ", v);
			if ((i % 4) == 3) DbgPrint("\n");
		}
		DbgPrint("[BAR0 as MSI-X table — 4 entries x 16 bytes]:\n");
		for (i = 0; i < 4; i++) {
			UINT32 alo = *(volatile UINT32*)((PUCHAR)deviceContext->Bar0BaseAddress + i*16 + 0);
			UINT32 ahi = *(volatile UINT32*)((PUCHAR)deviceContext->Bar0BaseAddress + i*16 + 4);
			UINT32 dat = *(volatile UINT32*)((PUCHAR)deviceContext->Bar0BaseAddress + i*16 + 8);
			UINT32 ctl = *(volatile UINT32*)((PUCHAR)deviceContext->Bar0BaseAddress + i*16 + 12);
			DbgPrint("[BAR0 MSIX] vec[%u] addr=0x%08x_%08x data=0x%08x ctrl=0x%x\n",
				i, ahi, alo, dat, ctl);
		}
	} else {
		DbgPrint("[BAR0] NOT MAPPED — Windows did not expose BAR0 as a memory resource\n");
	}

	DbgPrint("[%s] Exit\n", __FUNCTION__);

	return STATUS_SUCCESS;
}

NTSTATUS npudriverSettingResourceInfo(WDFDEVICE Device, WDFCMRESLIST ResourceList)
{
	PDEVICE_CONTEXT deviceContext = DeviceGetContext(Device);
	ULONG count;
	ULONG i;
	PCM_PARTIAL_RESOURCE_DESCRIPTOR descriptor;

	if (ResourceList == NULL) {
		DbgPrint("[%s] ResourceList is NULL\n", __FUNCTION__);
		return STATUS_INVALID_PARAMETER;
	}

	count = WdfCmResourceListGetCount(ResourceList);
	DbgPrint("[%s] Total Resources: %lu\n", __FUNCTION__, count);

	for (i = 0; i < count; i++) {
		descriptor = WdfCmResourceListGetDescriptor(ResourceList, i);

		if (descriptor == NULL) {
			continue;
		}

		DbgPrint("[%s] Resource[%lu]: Type=%lu (raw flags=0x%x)\n",
			__FUNCTION__, i, descriptor->Type, descriptor->Flags);

		// Type=129 = CmResourceTypeNonArbitrated(0x80) | CmResourceTypeMemory(0x01).
		// PCI bus driver exposes MSI-X table location via raw/non-arbitrated memory
		// descriptors. Map and dump these — they likely contain the MSI-X table
		// the chip uses but Windows places it in a region we never mapped.
		if (descriptor->Type == 129 || descriptor->Type == CmResourceTypeMemoryLarge) {
			DbgPrint("[%s] Resource[%lu] (raw mem) Start=0x%llx Length=0x%lx Flags=0x%x\n",
				__FUNCTION__, i,
				descriptor->u.Memory.Start.QuadPart,
				descriptor->u.Memory.Length,
				descriptor->Flags);
			// Try mapping a small one (likely MSI-X table size: 4 entries * 16 = 64 bytes
			// or PBA ~ also small)
			if (descriptor->u.Memory.Length > 0 && descriptor->u.Memory.Length <= 0x10000) {
				PVOID raw = MmMapIoSpace(descriptor->u.Memory.Start,
				                         descriptor->u.Memory.Length, MmNonCached);
				if (raw) {
					ULONG j;
					ULONG nDw = (ULONG)min(descriptor->u.Memory.Length / 4, 32);
					DbgPrint("[Res%lu] mapped @%p, dumping %lu dwords:\n", i, raw, nDw);
					for (j = 0; j < nDw; j++) {
						UINT32 v = *(volatile UINT32*)((PUCHAR)raw + j*4);
						if ((j % 4) == 0) DbgPrint("[Res%lu+0x%03x] ", i, j*4);
						DbgPrint("%08x ", v);
						if ((j % 4) == 3) DbgPrint("\n");
					}
					MmUnmapIoSpace(raw, descriptor->u.Memory.Length);
				} else {
					DbgPrint("[Res%lu] MmMapIoSpace failed\n", i);
				}
			}
		}

		switch (descriptor->Type) {
		case CmResourceTypeMemory:
			DbgPrint(" (Memory) Start=0x%llx Length=0x%lx\n",
				descriptor->u.Memory.Start.QuadPart,
				descriptor->u.Memory.Length);

			// save BAR2
			if (descriptor->u.Memory.Length == 0x100000) {
				deviceContext->Bar2Length = descriptor->u.Memory.Length;

				deviceContext->Bar2BaseAddress = MmMapIoSpace(
					descriptor->u.Memory.Start,
					descriptor->u.Memory.Length,
					MmNonCached
				);
			}
			// save BAR0 (length 0x4000) — diagnostic. Standard PCI MSI-X table likely lives here.
			else if (descriptor->u.Memory.Length == 0x4000) {
				deviceContext->Bar0Length = descriptor->u.Memory.Length;
				deviceContext->Bar0BaseAddress = MmMapIoSpace(
					descriptor->u.Memory.Start,
					descriptor->u.Memory.Length,
					MmNonCached
				);
				DbgPrint("[%s] BAR0 mapped: VA=%p PA=0x%llx len=0x%x\n",
					__FUNCTION__, deviceContext->Bar0BaseAddress,
					descriptor->u.Memory.Start.QuadPart, descriptor->u.Memory.Length);
			}

			break;

		case CmResourceTypePort:
			DbgPrint(" (I/O Port) Start=0x%llx Length=0x%lx\n",
				descriptor->u.Port.Start.QuadPart,
				descriptor->u.Port.Length);
			break;

		case CmResourceTypeInterrupt:
			DbgPrint(" (Interrupt) Level=%lu Vector=%lu Affinity=0x%lx\n",
				descriptor->u.Interrupt.Level,
				descriptor->u.Interrupt.Vector,
				(ULONG)descriptor->u.Interrupt.Affinity);
			break;

		case CmResourceTypeDma:
			DbgPrint(" (DMA) Channel=%lu Port=%lu\n",
				descriptor->u.Dma.Channel,
				descriptor->u.Dma.Port);
			break;

		default:
			DbgPrint(" (Unknown Type)\n");
			break;
		}
	}

	return STATUS_SUCCESS;
}

NTSTATUS
npudriverEvtDeviceReleaseHardware(
	_In_ WDFDEVICE Device,
	_In_ WDFCMRESLIST ResourcesTranslated
)
{
	UNREFERENCED_PARAMETER(ResourcesTranslated);

	PDEVICE_CONTEXT deviceContext = DeviceGetContext(Device);

	PAGED_CODE();

	DbgPrint("[%s] Entry\n", __FUNCTION__);

	// Cleanup page table
	ApexPageTableCleanup(Device);

	if (deviceContext->DescRingBase != NULL) {
		ExFreePoolWithTag(deviceContext->DescRingBase, 'DRNG');
		deviceContext->DescRingBase = NULL;
	}
	if (deviceContext->StatusBlockBase != NULL) {
		ExFreePoolWithTag(deviceContext->StatusBlockBase, 'SBLK');
		deviceContext->StatusBlockBase = NULL;
	}

	if (deviceContext->Bar2BaseAddress != NULL) {
		MmUnmapIoSpace(deviceContext->Bar2BaseAddress, deviceContext->Bar2Length);
		deviceContext->Bar2BaseAddress = NULL;
		DbgPrint("[%s] BAR2 unmapped\n", __FUNCTION__);
	}

	if (deviceContext->Bar0BaseAddress != NULL) {
		MmUnmapIoSpace(deviceContext->Bar0BaseAddress, deviceContext->Bar0Length);
		deviceContext->Bar0BaseAddress = NULL;
		DbgPrint("[%s] BAR0 unmapped\n", __FUNCTION__);
	}

	deviceContext->DeviceStatus = DEVICE_STATUS_DEAD;

	DbgPrint("[%s] Exit\n", __FUNCTION__);

	return STATUS_SUCCESS;
}

VOID
npudriverEvtFileCleanup(
	_In_ WDFFILEOBJECT FileObject
)
{
	WDFDEVICE device = WdfFileObjectGetDevice(FileObject);
	PDEVICE_CONTEXT deviceContext = DeviceGetContext(device);

	DbgPrint("[%s] File cleanup called\n", __FUNCTION__);

	// Safety check: unlock pages if they weren't properly unlocked
	if (deviceContext->LockedModelMdl != NULL) {
		DbgPrint("[%s] WARNING: Locked MDL detected during file cleanup\n", __FUNCTION__);
		DbgPrint("[%s] Unlocking %llu bytes...\n", __FUNCTION__, deviceContext->LockedModelSize);

		MmUnlockPages(deviceContext->LockedModelMdl);
		IoFreeMdl(deviceContext->LockedModelMdl);

		deviceContext->LockedModelMdl = NULL;
		deviceContext->LockedModelSize = 0;

		DbgPrint("[%s] Locked pages cleaned up successfully\n", __FUNCTION__);
	}

	if (deviceContext->InferInputMdl != NULL) {
		DbgPrint("[%s] WARNING: InferInputMdl still locked at cleanup\n", __FUNCTION__);
		MmUnlockPages(deviceContext->InferInputMdl);
		IoFreeMdl(deviceContext->InferInputMdl);
		deviceContext->InferInputMdl = NULL;
	}

	if (deviceContext->InferOutputMdl != NULL) {
		DbgPrint("[%s] WARNING: InferOutputMdl still locked at cleanup\n", __FUNCTION__);
		MmUnlockPages(deviceContext->InferOutputMdl);
		IoFreeMdl(deviceContext->InferOutputMdl);
		deviceContext->InferOutputMdl = NULL;
	}

	if (deviceContext->InferScratchMdl != NULL) {
		DbgPrint("[%s] WARNING: InferScratchMdl still locked at cleanup\n", __FUNCTION__);
		MmUnlockPages(deviceContext->InferScratchMdl);
		IoFreeMdl(deviceContext->InferScratchMdl);
		deviceContext->InferScratchMdl = NULL;
	}

	// Cleanup cached parameters (kept mapped throughout file handle lifetime)
	if (deviceContext->CachedParamMdl != NULL) {
		PVOID bar2 = deviceContext->Bar2BaseAddress;
		UINT32 i;
		DbgPrint("[%s] Releasing cached parameters: PTE[%u..%u] (%u pages)\n",
			__FUNCTION__,
			deviceContext->CachedParamPteIdx,
			deviceContext->CachedParamPteIdx + deviceContext->CachedParamPageCount - 1,
			deviceContext->CachedParamPageCount);

		if (bar2 != NULL) {
			WdfSpinLockAcquire(deviceContext->PageTableLock);
			for (i = 0; i < deviceContext->CachedParamPageCount; i++) {
				apex_write_register(bar2,
					APEX_REG_PAGE_TABLE + ((deviceContext->CachedParamPteIdx + i) * 8), 0);
			}
			WdfSpinLockRelease(deviceContext->PageTableLock);
		}

		MmUnlockPages(deviceContext->CachedParamMdl);
		IoFreeMdl(deviceContext->CachedParamMdl);
		deviceContext->CachedParamMdl = NULL;
		deviceContext->CachedParamPteIdx = 0;
		deviceContext->CachedParamPageCount = 0;
	}

	// Cleanup cached PARAM bitstream (kept mapped for IOCTL_INFER_WITH_PARAM)
	if (deviceContext->CachedParamBitstreamMdl != NULL) {
		PVOID bar2 = deviceContext->Bar2BaseAddress;
		UINT32 i;
		DbgPrint("[%s] Releasing cached PARAM bitstream: PTE[%u..%u] (%u pages, MDL=%p)\n",
			__FUNCTION__,
			deviceContext->CachedParamBitstreamPteIdx,
			deviceContext->CachedParamBitstreamPteIdx + deviceContext->CachedParamBitstreamPageCount - 1,
			deviceContext->CachedParamBitstreamPageCount,
			deviceContext->CachedParamBitstreamMdl);

		if (bar2 != NULL) {
			WdfSpinLockAcquire(deviceContext->PageTableLock);
			for (i = 0; i < deviceContext->CachedParamBitstreamPageCount; i++) {
				apex_write_register(bar2,
					APEX_REG_PAGE_TABLE + ((deviceContext->CachedParamBitstreamPteIdx + i) * 8), 0);
			}
			WdfSpinLockRelease(deviceContext->PageTableLock);
		}

		MmUnlockPages(deviceContext->CachedParamBitstreamMdl);
		IoFreeMdl(deviceContext->CachedParamBitstreamMdl);
		deviceContext->CachedParamBitstreamMdl = NULL;
		deviceContext->CachedParamBitstreamSize = 0;
		deviceContext->CachedParamBitstreamPteIdx = 0;
		deviceContext->CachedParamBitstreamPageCount = 0;
		deviceContext->CachedParamBitstreamDeviceVA = 0;
	}
}

// Coral

VOID npudriverReadTemperature(WDFDEVICE Device)
{
	PDEVICE_CONTEXT deviceContext = DeviceGetContext(Device);

	if (deviceContext->Bar2BaseAddress == NULL) {
		DbgPrint("[%s] Bar2BaseAddress is NULL", __FUNCTION__);
		return;
	}

	PVOID tempRegAddr = (PVOID)((PCHAR)deviceContext->Bar2BaseAddress + APEX_REG_OMC0_D0);

	ULONG rawTemp = READ_REGISTER_ULONG((PULONG)tempRegAddr);

	DbgPrint("[%s] Register(0x01a0d0) Raw Value: 0x%08X\n", __FUNCTION__, rawTemp);
}

BOOLEAN npudriverEvtInterruptIsr(WDFINTERRUPT Interrupt, ULONG MessageID)
{
	WDFDEVICE device = WdfInterruptGetDevice(Interrupt);
	PDEVICE_CONTEXT pDevContext = DeviceGetContext(device);

	// Unconditional log: even spurious calls (pending==0) get traced so we can
	// verify whether MSI-X is delivering anything at all. Counter survives
	// across calls so we see total interrupt fire count.
	InterlockedIncrement(&pDevContext->IsrCallCount);
	LONG callNum = pDevContext->IsrCallCount;

	if (pDevContext->Bar2BaseAddress == NULL) {
		DbgPrint("[ISR#%d] called with MessageID=%lu but BAR2 not mapped — return FALSE\n",
			callNum, MessageID);
		return FALSE;
	}

	UINT64 pending = apex_read_register(pDevContext->Bar2BaseAddress,
									   APEX_REG_WIRE_INT_PENDING);
	UINT64 iqIntStatus = apex_read_register(pDevContext->Bar2BaseAddress,
											APEX_REG_INSTR_QUEUE_INT_STATUS);

	// Always log entry — pending==0 case included
	DbgPrint("[ISR#%d] MessageID=%lu pending=0x%llx iq_int=0x%llx\n",
		callNum, MessageID, pending, iqIntStatus);

	if (pending == 0) {
		// Not our interrupt (or already cleared). Tell KMDF to try other handlers.
		return FALSE;
	}

	// Ack BOTH the source (IQ_INT_STATUS) and the aggregator (WIRE_INT_PENDING)
	// inside the ISR — must happen before we leave DIRQL or the chip re-asserts
	// MSI-X immediately and we get an interrupt storm.
	//
	// Order matters: clear the source first (IQ_INT_STATUS) so the level-driven
	// aggregator drops; then clear the aggregator latch (WIRE_INT_PENDING).
	// IQ_INT_STATUS turns out to be W1C on this chip — writing 0 leaves the bit
	// set and chip re-fires forever.
	if (iqIntStatus != 0) {
		apex_write_register(pDevContext->Bar2BaseAddress,
			APEX_REG_INSTR_QUEUE_INT_STATUS, iqIntStatus);
	}
	apex_write_register(pDevContext->Bar2BaseAddress,
					   APEX_REG_WIRE_INT_PENDING, pending);

	WdfInterruptQueueDpcForIsr(Interrupt);
	return TRUE;
}

NTSTATUS npudriverEvtInterruptEnable(WDFINTERRUPT Interrupt, WDFDEVICE Device)
{
	PDEVICE_CONTEXT pDevContext = DeviceGetContext(Device);
	DbgPrint("[%s] KMDF enabling interrupt\n", __FUNCTION__);

	// KMDF 1.15's WDF_INTERRUPT_INFO does NOT expose MessageAddress/MessageData
	// (those fields exist in newer KMDF but not 1.15). We can still see if the
	// interrupt is MessageSignaled (true=MSI-X, false=line-based) and which message
	// number Windows assigned. If MessageSignaled=false, we know MSI-X is not even
	// active — that alone is critical info.
	WDF_INTERRUPT_INFO info;
	WDF_INTERRUPT_INFO_INIT(&info);
	WdfInterruptGetInfo(Interrupt, &info);

	DbgPrint("[ENABLE] InterruptInfo: MessageSignaled=%d Vector=%lu MessageNumber=%lu Irql=%u Mode=%d Polarity=%d\n",
		info.MessageSignaled, info.Vector, info.MessageNumber,
		info.Irql, info.Mode, info.Polarity);

	if (!info.MessageSignaled) {
		DbgPrint("[ENABLE] WARNING: interrupt is LINE-BASED, not MSI-X — chip's MSI-X table will never be populated by Windows\n");
	}

	if (pDevContext->Bar2BaseAddress != NULL) {
		PVOID bar2 = pDevContext->Bar2BaseAddress;
		DbgPrint("[ENABLE] WIRE_INT_MASK=0x%llx WIRE_INT_PENDING=0x%llx IQ_INT_STATUS=0x%llx IQ_INT_CTRL=0x%llx SC_HOST_INT_CTRL=0x%llx\n",
			apex_read_register(bar2, APEX_REG_WIRE_INT_MASK),
			apex_read_register(bar2, APEX_REG_WIRE_INT_PENDING),
			apex_read_register(bar2, APEX_REG_INSTR_QUEUE_INT_STATUS),
			apex_read_register(bar2, APEX_REG_INSTR_QUEUE_INT_CONTROL),
			apex_read_register(bar2, APEX_REG_SC_HOST_INT_CONTROL));
		DbgPrint("[ENABLE] TOP_LEVEL_INTVECCTL=0x%llx INSTR_QUEUE_INTVECCTL=0x%llx SC_HOST_INTVECCTL=0x%llx\n",
			apex_read_register(bar2, APEX_REG_TOP_LEVEL_INTVECCTL),
			apex_read_register(bar2, APEX_REG_INSTR_QUEUE_INTVECCTL),
			apex_read_register(bar2, APEX_REG_SC_HOST_INTVECCTL));

		// Snapshot the chip's MSI-X table again at this point — by now Windows
		// MAY have populated it (timing matters). If still 0, Windows really
		// isn't writing it; we'd need to read PCI config space MSI-X capability
		// and find where Windows stashed message addr/data.
		ULONG i;
		for (i = 0; i < APEX_INTERRUPT_COUNT; i++) {
			ULONG ent = APEX_REG_KERNEL_HIB_MSIX_TABLE + i * APEX_MSIX_VECTOR_SIZE;
			UINT32 alo = apex_read_register_32(bar2, ent + 0);
			UINT32 ahi = apex_read_register_32(bar2, ent + 4);
			UINT32 dat = apex_read_register_32(bar2, ent + 8);
			UINT32 ctl = apex_read_register_32(bar2, ent + 12);
			DbgPrint("[ENABLE-MSIX] vec[%lu] addr=0x%08x_%08x data=0x%08x ctrl=0x%x\n",
				i, ahi, alo, dat, ctl);
		}
	}

	return STATUS_SUCCESS;
}

NTSTATUS npudriverEvtInterruptDisable(WDFINTERRUPT Interrupt, WDFDEVICE Device)
{
	UNREFERENCED_PARAMETER(Interrupt);
	UNREFERENCED_PARAMETER(Device);
	DbgPrint("[%s] KMDF disabling MSI-X\n", __FUNCTION__);
	return STATUS_SUCCESS;
}

VOID npudriverEvtInterruptDpc(WDFINTERRUPT Interrupt, WDFOBJECT AssociatedObject)
{
	UNREFERENCED_PARAMETER(AssociatedObject);

	WDFDEVICE device = WdfInterruptGetDevice(Interrupt);
	PDEVICE_CONTEXT pDevContext = DeviceGetContext(device);

	DbgPrint("[%s] DPC: Inference complete, unlocking pages\n", __FUNCTION__);
	// (interrupt ack moved into ISR to prevent storm — clearing only in DPC
	//  let the chip re-fire MSI-X before DPC ran)

	if (pDevContext->InferInputMdl != NULL) {
		MmUnlockPages(pDevContext->InferInputMdl);
		IoFreeMdl(pDevContext->InferInputMdl);
		pDevContext->InferInputMdl = NULL;
		DbgPrint("[%s] Input MDL unlocked\n", __FUNCTION__);
	}

	if (pDevContext->InferOutputMdl != NULL) {
		MmUnlockPages(pDevContext->InferOutputMdl);
		IoFreeMdl(pDevContext->InferOutputMdl);
		pDevContext->InferOutputMdl = NULL;
		DbgPrint("[%s] Output MDL unlocked\n", __FUNCTION__);
	}

	if (pDevContext->InferScratchMdl != NULL) {
		MmUnlockPages(pDevContext->InferScratchMdl);
		IoFreeMdl(pDevContext->InferScratchMdl);
		pDevContext->InferScratchMdl = NULL;
		DbgPrint("[%s] Scratch MDL unlocked\n", __FUNCTION__);
	}

	KeSetEvent(&pDevContext->InferCompleteEvent, IO_NO_INCREMENT, FALSE);
	DbgPrint("[%s] InferCompleteEvent set\n", __FUNCTION__);
}

// ============================================================================
// PCI configuration space walk to find the standard MSI-X capability (id=0x11).
// Returns BIR (0..5 = BAR0..BAR5) and Table Offset within that BAR. This tells
// us authoritatively where Windows placed the MSI-X table — without guessing.
//
// If chip's BAR2+0x46800 table is empty after this, it's because Windows wrote
// addr/data into the BAR/offset reported here. We can then either (a) mirror
// those values into chip's BAR2+0x46800, or (b) point chip at the real table.
// ============================================================================
NTSTATUS npudriverDumpMsixCapability(WDFDEVICE Device)
{
	BUS_INTERFACE_STANDARD bus = { 0 };
	NTSTATUS status;
	UCHAR hdr[64];
	UCHAR cap[16];
	UCHAR capPtr;
	int safety;
	UINT16 vendorId, deviceId, statusReg;
	ULONG bytes;

	status = WdfFdoQueryForInterface(
		Device,
		&GUID_BUS_INTERFACE_STANDARD,
		(PINTERFACE)&bus,
		sizeof(BUS_INTERFACE_STANDARD),
		1, NULL);
	if (!NT_SUCCESS(status)) {
		DbgPrint("[MSIX-CAP] WdfFdoQueryForInterface(BUS_INTERFACE_STANDARD) failed 0x%x\n", status);
		return status;
	}

	bytes = bus.GetBusData(bus.Context, PCI_WHICHSPACE_CONFIG, hdr, 0, 64);
	if (bytes < 64) {
		DbgPrint("[MSIX-CAP] GetBusData hdr failed (got %lu bytes)\n", bytes);
		bus.InterfaceDereference(bus.Context);
		return STATUS_UNSUCCESSFUL;
	}

	vendorId  = *(UINT16*)(hdr + 0x00);
	deviceId  = *(UINT16*)(hdr + 0x02);
	statusReg = *(UINT16*)(hdr + 0x06);
	capPtr    = hdr[0x34];

	DbgPrint("[MSIX-CAP] PCI VID=0x%04x DID=0x%04x Status=0x%04x CapPtr=0x%02x\n",
		vendorId, deviceId, statusReg, capPtr);

	if (!(statusReg & 0x10)) {  // PCI_STATUS_CAPABILITIES_LIST = 0x10
		DbgPrint("[MSIX-CAP] device reports no capabilities list\n");
		bus.InterfaceDereference(bus.Context);
		return STATUS_NOT_FOUND;
	}

	safety = 32;
	while (capPtr && capPtr != 0xFF && safety-- > 0) {
		bytes = bus.GetBusData(bus.Context, PCI_WHICHSPACE_CONFIG, cap, capPtr, 12);
		if (bytes < 12) {
			DbgPrint("[MSIX-CAP] read cap@0x%02x failed (got %lu)\n", capPtr, bytes);
			break;
		}
		DbgPrint("[MSIX-CAP] cap@0x%02x id=0x%02x next=0x%02x msgctrl=0x%04x dw1=0x%08x dw2=0x%08x\n",
			capPtr, cap[0], cap[1],
			*(UINT16*)(cap + 2),
			*(UINT32*)(cap + 4),
			*(UINT32*)(cap + 8));

		if (cap[0] == 0x11) {  // PCI_CAPABILITY_ID_MSIX
			UINT16 msgCtrl = *(UINT16*)(cap + 2);
			UINT32 tblBO   = *(UINT32*)(cap + 4);
			UINT32 pbaBO   = *(UINT32*)(cap + 8);
			UINT8  tblBir  = (UINT8)(tblBO & 0x7);
			UINT32 tblOff  = tblBO & ~0x7u;
			UINT8  pbaBir  = (UINT8)(pbaBO & 0x7);
			UINT32 pbaOff  = pbaBO & ~0x7u;
			UINT16 tblSize = (msgCtrl & 0x7FF) + 1;
			BOOLEAN enabled = (msgCtrl & 0x8000) ? TRUE : FALSE;
			BOOLEAN funcMask= (msgCtrl & 0x4000) ? TRUE : FALSE;

			DbgPrint("[MSIX-CAP] *** MSI-X *** size=%u enabled=%u funcMask=%u\n",
				tblSize, enabled, funcMask);
			DbgPrint("[MSIX-CAP]   Table  BIR=%u Offset=0x%x  (BAR%u + 0x%x)\n",
				tblBir, tblOff, tblBir, tblOff);
			DbgPrint("[MSIX-CAP]   PBA    BIR=%u Offset=0x%x  (BAR%u + 0x%x)\n",
				pbaBir, pbaOff, pbaBir, pbaOff);
			break;
		}
		capPtr = cap[1];
	}

	bus.InterfaceDereference(bus.Context);
	return STATUS_SUCCESS;
}

