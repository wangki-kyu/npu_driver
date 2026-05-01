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

		// Extended-VA bulk pool — allocated in ApexPageTableInit.
		deviceContext->ExtPoolKva  = NULL;
		deviceContext->ExtPoolPa   = 0;
		deviceContext->ExtPoolSize = 0;

		// Output bounce buffer state — inactive on boot.
		deviceContext->OutputBounceKva    = NULL;
		deviceContext->OutputBouncePa     = 0;
		deviceContext->OutputBounceSize   = 0;
		deviceContext->OutputBounceActive = FALSE;

		// Cached parameter MDL (IOCTL_PARAM_CACHE 가 채움)
		deviceContext->CachedParamMdl = NULL;
		deviceContext->CachedParamDeviceVA = 0;
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

	// === HIB credit dump (PRE-RESET) — diagnose whether GCB reset wipes ===
	// these the same way it wipes the MSI-X table.  Hypothesis: POR default
	// is non-zero, RAM shutdown clears them, libedgetpu/gasket never write
	// them, so after reset OUTFEED has no outbound DMA credit and host
	// writes are silently dropped.  Compare values across the 4 dump
	// points to confirm.
	{
		PVOID b2 = deviceContext->Bar2BaseAddress;
		UINT32 c0 = apex_read_register_32(b2, APEX_REG_HIB_INSTRUCTION_CREDITS);
		UINT32 c1 = apex_read_register_32(b2, APEX_REG_HIB_INPUT_ACTV_CREDITS);
		UINT32 c2 = apex_read_register_32(b2, APEX_REG_HIB_PARAM_CREDITS);
		UINT32 c3 = apex_read_register_32(b2, APEX_REG_HIB_OUTPUT_ACTV_CREDITS);
		DbgPrint("[CREDITS@PRE-RESET]   instr=0x%08x input=0x%08x param=0x%08x output=0x%08x\n",
			c0, c1, c2, c3);
	}

	// Reset and quit-reset sequence to enable GCB (Global Clock Block)
	// This is required for the scalar core to be operational
	DbgPrint("[%s] Starting GCB reset sequence (libedgetpu-style: no RAM shutdown force)\n", __FUNCTION__);
	apex_rmw_register_32(deviceContext->Bar2BaseAddress, APEX_REG_SCU_2, 0x1, 2, 2);  // Enable GCB reset
	apex_rmw_register_32(deviceContext->Bar2BaseAddress, APEX_REG_SCU_2, 0x1, 2, 18); // Enable clock gate
	// EXPERIMENT: do NOT force SCU_3 bit14 (RAM shutdown). libedgetpu's EnableReset
	// only writes rg_force_sleep=0x3 (SCU_3 bits[22:23]) and polls cur_pwr_state==0x2.
	// Forcing RAM shutdown wipes chip-internal SRAM state (suspected to wipe OUTFEED
	// engine's internal buffers/state, killing outbound DMA after RunControl=1).
	{
		UINT32 scu3Before = apex_read_register_32(deviceContext->Bar2BaseAddress, APEX_REG_SCU_3);
		DbgPrint("[%s] SCU_3 before sleep-mode entry: 0x%08x (skipping force RAM shutdown)\n",
			__FUNCTION__, scu3Before);
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

	DbgPrint("[%s] Starting GCB quit-reset sequence (no RAM enable polling)\n", __FUNCTION__);
	// rg_gated_gcb (SCU_2 bits[19:18]) — libedgetpu values:
	//   0x0 = deprecated, 0x1 = hardware clock gated, 0x2 = no clock gating (force on)
	// 0x0 (deprecated) puts GCB in undefined gating state — large bitstreams (INFER)
	// stall mid-execution. 0x2 (force on) matches DisableHardwareClockGate().
	apex_rmw_register_32(deviceContext->Bar2BaseAddress, APEX_REG_SCU_2, 0x2, 2, 18); // rg_gated_gcb = no clock gating
	apex_rmw_register_32(deviceContext->Bar2BaseAddress, APEX_REG_SCU_2, 0x2, 2, 2);  // Exit reset
	// EXPERIMENT: skip RAM enable polling (bit6) — we never forced RAM shutdown,
	// so there's nothing to wait for here. libedgetpu's QuitReset polls cur_pwr_state==0x0
	// instead, which is checked separately in the SCALAR_RUN_CONTROL polling below.
	{
		UINT32 scu3After = apex_read_register_32(deviceContext->Bar2BaseAddress, APEX_REG_SCU_3);
		DbgPrint("[%s] SCU_3 after quit-reset: 0x%08x\n", __FUNCTION__, scu3After);
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

	// === HIB credit dump (POST-RESET) — should we see zeros here? ===
	{
		PVOID b2 = deviceContext->Bar2BaseAddress;
		UINT32 c0 = apex_read_register_32(b2, APEX_REG_HIB_INSTRUCTION_CREDITS);
		UINT32 c1 = apex_read_register_32(b2, APEX_REG_HIB_INPUT_ACTV_CREDITS);
		UINT32 c2 = apex_read_register_32(b2, APEX_REG_HIB_PARAM_CREDITS);
		UINT32 c3 = apex_read_register_32(b2, APEX_REG_HIB_OUTPUT_ACTV_CREDITS);
		DbgPrint("[CREDITS@POST-RESET]  instr=0x%08x input=0x%08x param=0x%08x output=0x%08x\n",
			c0, c1, c2, c3);
	}

	// Re-write HIB MMU config after GCB reset — these registers may have been
	// cleared by the reset even though they were written in ApexPageTableInit().
	// (Working user-mode log doesn't write these — gasket kernel does it. We keep
	//  it because we ARE the kernel side and our GCB reset wipes them.)
	{
		PVOID bar2 = deviceContext->Bar2BaseAddress;
		apex_write_register(bar2, APEX_REG_PAGE_TABLE_SIZE, (UINT64)APEX_PAGE_TABLE_ENTRIES);
		apex_write_register(bar2, APEX_REG_EXTENDED_TABLE, 6144);
		DbgPrint("[%s] PAGE_TABLE_SIZE=%u EXTENDED_TABLE=6144 re-written after GCB reset\n",
			__FUNCTION__, APEX_PAGE_TABLE_ENTRIES);

		// Re-pre-fill extended chip PTE registers [6144..8191] so every
		// extended slot points at its 4 KB sub-region of the bulk pool with
		// valid bit set.  GCB reset wipes these registers even though our
		// host-side pool memory survives.
		if (deviceContext->ExtPoolKva != NULL && deviceContext->ExtPoolPa != 0) {
			UINT32 i;
			for (i = 0; i < 2048u; i++) {
				UINT64 subPa = deviceContext->ExtPoolPa + ((UINT64)i << PAGE_SHIFT);
				apex_write_register(bar2,
					APEX_REG_PAGE_TABLE + ((6144u + i) * 8),
					subPa | 0x1ULL);
			}
			DbgPrint("[%s] Extended chip PTE [6144..8191] re-pointed at bulk pool (PA=0x%llx)\n",
				__FUNCTION__, deviceContext->ExtPoolPa);
		}
	}

	// =====================================================================
	// CHIP INIT — match working libedgetpu user-mode register sequence verbatim.
	// All register writes/order taken directly from a working coral.sys+libedgetpu
	// trace. Anything libedgetpu does NOT write is left at chip POR default.
	//
	// Removed (was in our previous code, but NOT in working trace):
	//   - IDLEGENERATOR (0x48508) write
	//   - all *_INTVECCTL writes (0x46018, 0x46020, 0x46028, 0x46030, 0x46038,
	//     0x46040, 0x46048) — POR default routing left intact
	//   - TOP_LEVEL_INT_CONTROL (0x486b0) write
	//   - WIRE_INT_MASK (0x48780) write
	//   - TILE_DEEP_SLEEP (manipulated only inside IOCTL_INFER, if at all)
	//
	// Kept (working trace omits because gasket kernel handles them; we ARE the
	// kernel, so we keep them):
	//   - GCB reset, MSI-X table SAVE/RESTORE/MASK clear
	//   - PAGE_TABLE_SIZE / EXTENDED_TABLE rewrite after GCB reset
	//   - PAGE_TABLE_INIT signal + MSIX_TABLE_INIT polling
	//   - DescRing/StatusBlock allocation + PTE[4096/4097] write (working trace: 0x1000000/0x1001000)
	// =====================================================================
	if (deviceContext->Bar2BaseAddress != NULL) {
		PVOID bar2 = deviceContext->Bar2BaseAddress;

		// Step 1: DMA_BURST_LIMITER = 0 (working trace step 2)
		apex_write_register(bar2, APEX_REG_DMA_BURST_LIMITER, 0);
		DbgPrint("[%s] DMA_BURST_LIMITER → 0\n", __FUNCTION__);

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
	// Maps to PTE slot 4096 (working trace), device VA = 4096 * 4KB = 0x1000000 (simple slot)
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
		deviceContext->DescRingDeviceVA = (UINT64)4096 * PAGE_SIZE;  // 0x1000000
		deviceContext->DescRingTail = 0;
		apex_write_register(deviceContext->Bar2BaseAddress,
			APEX_REG_PAGE_TABLE + (4096 * 8), descPhys.QuadPart | 1);
		{
			UINT64 rb = apex_read_register(deviceContext->Bar2BaseAddress,
				APEX_REG_PAGE_TABLE + (4096 * 8));
			DbgPrint("[%s] DescRing: VA=%p PA=0x%llx DeviceVA=0x%llx PTE[4096] write=0x%llx readback=0x%llx %s\n",
				__FUNCTION__, deviceContext->DescRingBase, descPhys.QuadPart,
				deviceContext->DescRingDeviceVA, descPhys.QuadPart | 1, rb,
				(rb == (UINT64)(descPhys.QuadPart | 1)) ? "OK" : "MISMATCH");
		}
	}

	// Allocate status block (4KB) — hardware DMA-writes completion info here
	// Maps to PTE slot 4097 (working trace), device VA = 4097 * 4KB = 0x1001000 (simple slot)
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
		deviceContext->StatusBlockDeviceVA = (UINT64)4097 * PAGE_SIZE;  // 0x1001000
		apex_write_register(deviceContext->Bar2BaseAddress,
			APEX_REG_PAGE_TABLE + (4097 * 8), sblkPhys.QuadPart | 1);
		{
			UINT64 rb = apex_read_register(deviceContext->Bar2BaseAddress,
				APEX_REG_PAGE_TABLE + (4097 * 8));
			DbgPrint("[%s] StatusBlock: VA=%p PA=0x%llx DeviceVA=0x%llx PTE[4097] write=0x%llx readback=0x%llx %s\n",
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
		UINT64 rb4096 = apex_read_register(deviceContext->Bar2BaseAddress, APEX_REG_PAGE_TABLE + (4096 * 8));
		UINT64 rb4097 = apex_read_register(deviceContext->Bar2BaseAddress, APEX_REG_PAGE_TABLE + (4097 * 8));
		DbgPrint("[%s] PTE[4096](DescRing)=0x%llx PTE[4097](StatusBlock)=0x%llx\n",
			__FUNCTION__, rb4096, rb4097);
		if ((rb4096 & 1) == 0) DbgPrint("[%s] WARNING: PTE[4096] not valid!\n", __FUNCTION__);
		if ((rb4097 & 1) == 0) DbgPrint("[%s] WARNING: PTE[4097] not valid!\n", __FUNCTION__);
	}

	// INSTR_QUEUE_CONTROL = 0x5 (enable + sb_wr_enable). Working trace writes
	// this once with the final value (no two-step). Then poll STATUS until bit0=1.
	apex_write_register(deviceContext->Bar2BaseAddress, APEX_REG_INSTR_QUEUE_CONTROL, 0x5);
	DbgPrint("[%s] INSTR_QUEUE_CONTROL → 0x5 (enable + sb_wr_enable)\n", __FUNCTION__);
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

	// Read SC_HOST_INT_COUNT baseline (working trace step 11) — also stored in
	// LastScHostIntCount at IOCTL_INFER pre-submit time, but we read it once here
	// for diagnostic logging.
	{
		UINT64 baseline = apex_read_register(deviceContext->Bar2BaseAddress, APEX_REG_SC_HOST_INT_COUNT);
		DbgPrint("[%s] SC_HOST_INT_COUNT baseline = 0x%llx\n", __FUNCTION__, baseline);
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

		// Working trace order (verbatim):
		//   Scalar-side RUN_CONTROL writes → TILE_CONFIG0 + readback → Tile-side
		//   RUN_CONTROL writes → STATUS_BLOCK_UPDATE=0 → SC_HOST_INT_CONTROL=0xF
		//   → INSTR_QUEUE_INT_CONTROL=1 → FATAL_ERR_INT_CONTROL=1
		//
		// TILE_DEEP_SLEEP write removed — working trace never touches it (left at POR).

		// Phase 1: scalar-side run controls (working trace order, NOT alphabetical).
		//   0x44018 = SCALAR
		//   0x44158 = AVDATA_POP
		//   0x44198 = PARAMETER_POP
		//   0x441d8 = INFEED
		//   0x44218 = OUTFEED
		apex_write_register_32(bar2, APEX_REG_SCALAR_RUN_CONTROL,        1);
		apex_write_register_32(bar2, APEX_REG_AVDATA_POP_RUN_CONTROL,    1);
		apex_write_register_32(bar2, APEX_REG_PARAMETER_POP_RUN_CONTROL, 1);
		apex_write_register_32(bar2, APEX_REG_INFEED_RUN_CONTROL,        1);
		apex_write_register_32(bar2, APEX_REG_OUTFEED_RUN_CONTROL,       1);

		// Phase 2: TILE_CONFIG0 broadcast + readback confirm.
		apex_write_register(bar2, APEX_REG_TILE_CONFIG0, 0x7F);
		{
			int tci;
			for (tci = 0; tci < 1000; tci++) {
				if (apex_read_register(bar2, APEX_REG_TILE_CONFIG0) == 0x7F) break;
				KeStallExecutionProcessor(10);
			}
			DbgPrint("[%s] TILE_CONFIG0=0x7F confirmed after %d polls\n", __FUNCTION__, tci);
		}

		// Phase 3: tile-side run controls (working trace order):
		//   0x400c0 = TILE_OP
		//   0x40150 = NARROW_TO_WIDE
		//   0x40110 = WIDE_TO_NARROW
		//   0x40250 = MESH_BUS0
		//   0x40298 = MESH_BUS1
		//   0x402e0 = MESH_BUS2
		//   0x40328 = MESH_BUS3
		//   0x40190 = RING_BUS_CONSUMER0
		//   0x401d0 = RING_BUS_CONSUMER1
		//   0x40210 = RING_BUS_PRODUCER
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

		// Phase 4: STATUS_BLOCK_UPDATE = 0 (must come AFTER all run controls per
		// working trace).  Disables periodic status block auto-write.
		apex_write_register(bar2, APEX_REG_STATUS_BLOCK_UPDATE, 0);
		DbgPrint("[%s] STATUS_BLOCK_UPDATE → 0\n", __FUNCTION__);

		// Phase 5: interrupt enables (working trace LAST step).
		//   0x486a0 = SC_HOST_INT_CONTROL  = 0xF (enable SC_HOST 0..3)
		//   0x485c0 = INSTR_QUEUE_INT_CTRL = 1
		//   0x486c0 = FATAL_ERR_INT_CTRL   = 1
		apex_write_register(bar2, APEX_REG_SC_HOST_INT_CONTROL,    0xF);
		apex_write_register(bar2, APEX_REG_INSTR_QUEUE_INT_CONTROL, 1);
		apex_write_register(bar2, APEX_REG_FATAL_ERR_INT_CONTROL,   1);
		DbgPrint("[%s] Interrupts enabled: SC_HOST=0xF IQ=1 FATAL=1\n", __FUNCTION__);
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

	// HIB credits at end-of-PrepareHardware — these registers are R/O status
	// counters maintained by the chip; we keep the dump for diagnostics only.
	{
		PVOID b2 = deviceContext->Bar2BaseAddress;
		UINT32 c0 = apex_read_register_32(b2, APEX_REG_HIB_INSTRUCTION_CREDITS);
		UINT32 c1 = apex_read_register_32(b2, APEX_REG_HIB_INPUT_ACTV_CREDITS);
		UINT32 c2 = apex_read_register_32(b2, APEX_REG_HIB_PARAM_CREDITS);
		UINT32 c3 = apex_read_register_32(b2, APEX_REG_HIB_OUTPUT_ACTV_CREDITS);
		DbgPrint("[CREDITS@PREPARE-END] instr=0x%08x input=0x%08x param=0x%08x output=0x%08x\n",
			c0, c1, c2, c3);
	}

	// EXPERIMENT: enable OUTPUT_ACTV queue.  Per-queue dump showed:
	//   [Q@out_actv] control=0x0  ← only OUTPUT queue is disabled
	//   [Q@in_actv]  control=0x3  (enable + sb_wr_enable, auto)
	//   [Q@param]    control=0x3  (enable + sb_wr_enable, auto)
	// Hypothesis: chip emits outbound writes for OUTFEED but they need an enabled
	// OUTPUT_ACTV queue to reach host RAM.  Match input_actv/param queues' default
	// 0x3 (enable bit0 + sb_wr_enable bit1).
	{
		PVOID b2 = deviceContext->Bar2BaseAddress;
		UINT32 outqBefore = apex_read_register_32(b2, APEX_REG_OUTQ_CONTROL);
		apex_write_register_32(b2, APEX_REG_OUTQ_CONTROL, 0x3);
		UINT32 outqAfter = apex_read_register_32(b2, APEX_REG_OUTQ_CONTROL);
		UINT32 outqStat  = apex_read_register_32(b2, APEX_REG_OUTQ_STATUS);
		DbgPrint("[Q@out_actv] enabled: control before=0x%x wrote=0x3 after=0x%x status=0x%x\n",
			outqBefore, outqAfter, outqStat);
	}

	// === PER-QUEUE CSR DUMP — diagnose chip-expected descriptor sizes & queue states ===
	// libedgetpu HostQueue::Open reads queue_descriptor_size to verify sizeof(Element).
	// Beagle has 4 queues (instruction / output_actv / input_actv / param); we only use
	// the instruction queue. If chip's expected descriptor_size != 16, our descriptors
	// are misaligned. If output_actv queue has non-default state (control/status),
	// that's a hidden mechanism we missed.
	{
		PVOID b2 = deviceContext->Bar2BaseAddress;
		UINT32 iqDesc = apex_read_register_32(b2, APEX_REG_IQ_DESCRIPTOR_SIZE);
		UINT32 iqMin  = apex_read_register_32(b2, APEX_REG_IQ_MINIMUM_SIZE);
		UINT32 iqMax  = apex_read_register_32(b2, APEX_REG_IQ_MAXIMUM_SIZE);
		DbgPrint("[Q@instr]    descriptor_size=%u minimum=%u maximum=%u\n",
			iqDesc, iqMin, iqMax);

		UINT32 outqCtrl = apex_read_register_32(b2, APEX_REG_OUTQ_CONTROL);
		UINT32 outqStat = apex_read_register_32(b2, APEX_REG_OUTQ_STATUS);
		UINT32 outqDesc = apex_read_register_32(b2, APEX_REG_OUTQ_DESCRIPTOR_SIZE);
		UINT32 outqMin  = apex_read_register_32(b2, APEX_REG_OUTQ_MINIMUM_SIZE);
		DbgPrint("[Q@out_actv] control=0x%x status=0x%x descriptor_size=%u minimum=%u\n",
			outqCtrl, outqStat, outqDesc, outqMin);

		UINT32 inqCtrl = apex_read_register_32(b2, APEX_REG_INQ_CONTROL);
		UINT32 inqStat = apex_read_register_32(b2, APEX_REG_INQ_STATUS);
		UINT32 inqDesc = apex_read_register_32(b2, APEX_REG_INQ_DESCRIPTOR_SIZE);
		UINT32 inqMin  = apex_read_register_32(b2, APEX_REG_INQ_MINIMUM_SIZE);
		DbgPrint("[Q@in_actv]  control=0x%x status=0x%x descriptor_size=%u minimum=%u\n",
			inqCtrl, inqStat, inqDesc, inqMin);

		UINT32 prqCtrl = apex_read_register_32(b2, APEX_REG_PARAMQ_CONTROL);
		UINT32 prqStat = apex_read_register_32(b2, APEX_REG_PARAMQ_STATUS);
		UINT32 prqDesc = apex_read_register_32(b2, APEX_REG_PARAMQ_DESCRIPTOR_SIZE);
		UINT32 prqMin  = apex_read_register_32(b2, APEX_REG_PARAMQ_MINIMUM_SIZE);
		DbgPrint("[Q@param]    control=0x%x status=0x%x descriptor_size=%u minimum=%u\n",
			prqCtrl, prqStat, prqDesc, prqMin);

		// Top-level arbiter registers — POR / pre-INFER state.
		UINT32 rArb = apex_read_register_32(b2, APEX_REG_READ_REQUEST_ARBITER);
		UINT32 wArb = apex_read_register_32(b2, APEX_REG_WRITE_REQUEST_ARBITER);
		UINT32 atArb = apex_read_register_32(b2, APEX_REG_ADDR_TRANSLATION_ARBITER);
		DbgPrint("[ARB-CFG@prepare-end] read_req=0x%x write_req=0x%x addr_trans=0x%x\n",
			rArb, wArb, atArb);
	}

	// AER baseline at end of PrepareHardware — establishes a known clean state.
	// Any non-zero AER bits captured here are pre-existing chip/PCIe issues, not
	// caused by inference.  We W1C inside the helper so subsequent snapshots
	// only show NEW errors that occurred during inference.
	npudriverDumpPciAer(Device, "prepare-end");

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

	// Drain ExtraLockedMdls FIRST — these are MDLs that were piled up by
	// repeated IOCTL_MAP_BUFFER calls (e.g. exe0 0x800000 + INFER bs 0x900000
	// in the same open) before any UNMAP / PARAM_CACHE handover.  If we don't
	// release them here, the process termination path will hit
	// PROCESS_HAS_LOCKED_PAGES BSOD.
	{
		UINT32 i;
		for (i = 0; i < deviceContext->ExtraLockedMdlCount; i++) {
			DbgPrint("[%s] Releasing extras[%u] MDL=%p\n",
				__FUNCTION__, i, deviceContext->ExtraLockedMdls[i]);
			if (deviceContext->ExtraLockedMdls[i] != NULL) {
				MmUnlockPages(deviceContext->ExtraLockedMdls[i]);
				IoFreeMdl(deviceContext->ExtraLockedMdls[i]);
				deviceContext->ExtraLockedMdls[i] = NULL;
			}
		}
		deviceContext->ExtraLockedMdlCount = 0;
	}

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

	// Cleanup cached parameters (kept mapped throughout file handle lifetime).
	// ApexPageTableUnmap dispatches on the VA's bit63: extended → zero the
	// host-side 2-level PT entries; simple → zero chip PTE registers directly.
	// Without this the chip retains stale PFNs that point into pages we are
	// about to unlock, which corrupts kernel memory on the next chip access.
	if (deviceContext->CachedParamMdl != NULL) {
		SIZE_T sz = (SIZE_T)deviceContext->CachedParamPageCount << PAGE_SHIFT;
		DbgPrint("[%s] Releasing cached parameters: VA=0x%llx (%u pages)\n",
			__FUNCTION__,
			deviceContext->CachedParamDeviceVA,
			deviceContext->CachedParamPageCount);

		ApexPageTableUnmap(device, deviceContext->CachedParamDeviceVA, sz);

		MmUnlockPages(deviceContext->CachedParamMdl);
		IoFreeMdl(deviceContext->CachedParamMdl);
		deviceContext->CachedParamMdl = NULL;
		deviceContext->CachedParamDeviceVA = 0;
		deviceContext->CachedParamPteIdx = 0;
		deviceContext->CachedParamPageCount = 0;
	}

	// Cleanup cached PARAM bitstream (kept mapped for IOCTL_INFER_WITH_PARAM).
	if (deviceContext->CachedParamBitstreamMdl != NULL) {
		SIZE_T sz = (SIZE_T)deviceContext->CachedParamBitstreamPageCount << PAGE_SHIFT;
		DbgPrint("[%s] Releasing cached PARAM bitstream: VA=0x%llx (%u pages, MDL=%p)\n",
			__FUNCTION__,
			deviceContext->CachedParamBitstreamDeviceVA,
			deviceContext->CachedParamBitstreamPageCount,
			deviceContext->CachedParamBitstreamMdl);

		ApexPageTableUnmap(device, deviceContext->CachedParamBitstreamDeviceVA, sz);

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
	UINT64 scHostCount = apex_read_register(pDevContext->Bar2BaseAddress,
											APEX_REG_SC_HOST_INT_COUNT);

	// Always log entry — pending==0 case included
	DbgPrint("[ISR#%d] MessageID=%lu pending=0x%llx iq_int=0x%llx sc_host_count=0x%llx\n",
		callNum, MessageID, pending, iqIntStatus, scHostCount);

	if (pending == 0) {
		// Not our interrupt (or already cleared). Tell KMDF to try other handlers.
		return FALSE;
	}

	// Capture pending bits BEFORE we W1C-ack them — DPC needs to know what fired.
	// APEX_WIRE_BIT_IQ_INT (0x1) alone == descriptors fetched but engines may
	// still be running.  APEX_WIRE_BIT_SC_HOST_0 (0x10) == SCALAR reached the
	// bitstream's host_interrupt 0 opcode = compiler-promised "INFER done".
	// APEX_WIRE_BIT_FATAL_ERR (0x1000) == HIB fatal error, abort the IOCTL.
	pDevContext->LastIsrWirePending = pending;
	InterlockedOr(&pDevContext->IsrSeenPendingBits, (LONG)(pending & 0xFFFFFFFF));

	// Ack the SOURCE registers first (level-driven), then the aggregator
	// (WIRE_INT_PENDING). Without this the chip re-asserts MSI-X immediately
	// and we storm.
	//
	// SC_HOST_INT_STATUS (0x486a8) — REAL INFER completion source. libedgetpu
	// writes 0xE here on every SC_HOST fire (W1C bits 1..3, leaving bit 0 for
	// the count register to latch). Without this ack the next INFER's SC_HOST_0
	// edge is lost and OUTFEED appears to never complete. This was missing.
	if (pending & APEX_WIRE_BITS_SC_HOST_ANY) {
		apex_write_register(pDevContext->Bar2BaseAddress,
			APEX_REG_SC_HOST_INT_STATUS, 0xE);
	}

	// IQ_INT_STATUS — libedgetpu writes 0 to clear (NOT W1C as our prior
	// comment claimed). Observed in the working runtime trace.
	if (iqIntStatus != 0) {
		apex_write_register(pDevContext->Bar2BaseAddress,
			APEX_REG_INSTR_QUEUE_INT_STATUS, 0);
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
	PVOID bar2 = pDevContext->Bar2BaseAddress;

	DbgPrint("[%s] DPC: Inference complete, unlocking pages\n", __FUNCTION__);
	// (interrupt ack moved into ISR to prevent storm — clearing only in DPC
	//  let the chip re-fire MSI-X before DPC ran)

	// ============================================================
	// DPC-time diagnostics — must run BEFORE we unlock the MDLs.
	// Once MDLs are unlocked the kernel VA mapping is gone and the
	// host pages can move/page-out, so this is our only window to
	// (a) read engine RunStatus right after completion,
	// (b) verify PTE readback still points at the right host PFNs,
	// (c) dump the first bytes OUTFEED actually wrote into host RAM.
	//
	// NB: APEX_WIRE_BIT_IQ_INT (0x1) alone fires when descriptors are
	// fetched, NOT when SCALAR finishes executing them.  The real "done"
	// signal is APEX_WIRE_BIT_SC_HOST_0 (0x10) — the SCALAR-issued
	// host_interrupt opcode at end-of-program (compiler places this AFTER
	// the OUTFEED drain barrier).  If SCALAR is still running AND we
	// haven't seen SC_HOST_0 yet, MUST NOT unlock — unlocking before
	// OUTFEED writes back causes inbound page faults.
	// ============================================================
	UINT32 scStat = 0, outStat = 0;
	if (bar2 != NULL) {
		UINT32 inStat;
		UINT32 avStat;
		UINT64 iqDone;
		UINT64 iqIntSt;
		UINT64 wirePend;
		UINT64 hibErr;
		UINT64 hibFirst;
		scStat   = apex_read_register_32(bar2, APEX_REG_SCALAR_RUN_STATUS);
		inStat   = apex_read_register_32(bar2, APEX_REG_INFEED_RUN_STATUS);
		outStat  = apex_read_register_32(bar2, APEX_REG_OUTFEED_RUN_STATUS);
		avStat   = apex_read_register_32(bar2, APEX_REG_AVDATA_POP_RUN_STATUS);
		iqDone   = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_COMPLETED_HEAD);
		iqIntSt  = apex_read_register(bar2, APEX_REG_INSTR_QUEUE_INT_STATUS);
		wirePend = apex_read_register(bar2, APEX_REG_WIRE_INT_PENDING);
		hibErr   = apex_read_register(bar2, APEX_REG_USER_HIB_ERROR_STATUS);
		hibFirst = apex_read_register(bar2, APEX_REG_USER_HIB_FIRST_ERROR);
		// page_fault_address (0x48738) is the unified fault-VA register —
		// captures the device VA the chip was walking when MMU rejected it,
		// regardless of direction (INFEED inbound vs OUTFEED outbound write).
		UINT64 faultVA = apex_read_register(bar2, APEX_REG_INFEED_PAGE_FAULT_ADDR);
		UINT64 firstTs = apex_read_register(bar2, APEX_REG_USER_HIB_FIRST_ERROR_TS);
		DbgPrint("[DPC-DIAG] SC=0x%x IN=0x%x OUT=0x%x AV=0x%x | IQ_DONE=%llu IQ_INT=0x%llx WIRE_NOW=0x%llx WIRE_ISR=0x%llx SEEN=0x%x | HIB_ERR=0x%llx HIB_FIRST=0x%llx FAULT_VA=0x%llx FIRST_TS=0x%llx\n",
			scStat, inStat, outStat, avStat,
			iqDone, iqIntSt, wirePend,
			pDevContext->LastIsrWirePending,
			(UINT32)pDevContext->IsrSeenPendingBits,
			hibErr, hibFirst, faultVA, firstTs);

		// AXI write-channel counters at DPC time.  Compare against the
		// pre-submit snapshot in [AXI@pre-submit]:
		//   - awIns/wIns increased  → chip issued outbound write TLPs
		//                              (root complex / IOMMU likely the wall)
		//   - awIns/wIns unchanged  → chip never even tried to write outbound
		//                              (chip-internal ATU / credit gate is the wall)
		// awOcc/wOcc reflect in-flight (un-acked) write requests.
		{
			UINT32 awIns = apex_read_register_32(bar2, APEX_REG_AXI_AW_CREDIT_SHIM_INSERTION);
			UINT32 wIns  = apex_read_register_32(bar2, APEX_REG_AXI_W_CREDIT_SHIM_INSERTION);
			UINT32 awOcc = apex_read_register_32(bar2, APEX_REG_AXI_AW_CREDIT_SHIM_OCCUPANCY);
			UINT32 wOcc  = apex_read_register_32(bar2, APEX_REG_AXI_W_CREDIT_SHIM_OCCUPANCY);
			DbgPrint("[DPC-AXI] aw_ins=0x%x w_ins=0x%x aw_occ=0x%x w_occ=0x%x\n",
				awIns, wIns, awOcc, wOcc);
		}

		// HIB credits at DPC time — does any non-OUTPUT credit drop on fault?
		// If yes → credits are dynamic; OUTPUT=0 means chip thinks our OUTPUT
		// VA is invalid.  If unchanged → credits are POR static and irrelevant.
		{
			UINT64 cInstr  = apex_read_register(bar2, 0x48740);
			UINT64 cInput  = apex_read_register(bar2, 0x48748);
			UINT64 cParam  = apex_read_register(bar2, 0x48750);
			UINT64 cOutput = apex_read_register(bar2, 0x48758);
			DbgPrint("[DPC-CREDITS] instr=0x%llx input=0x%llx param=0x%llx output=0x%llx\n",
				cInstr, cInput, cParam, cOutput);
		}

		// NOTE: PCI config-space read requires PASSIVE_LEVEL (WdfFdoQueryForInterface).
		// DPC runs at DISPATCH_LEVEL.  PCI snapshot is collected by IOCTL post-INFER
		// instead — see [PCIE@post-infer] / [PCI-CMD@post-infer] in Queue.c.
		// hint: SC=0 OUT=0 -> all engines parked normally (real done)
		//       SC=1 -> still running, this DPC is premature, defer
		//       SC=4 (kHalted) -> halted, error completion
		//       HIB_ERR bit 0  -> page fault at HIB_FIRST device VA
	}

	// Status block dump — chip DMA-writes progress here.  Worth seeing once
	// per DPC even when premature, since values may transition while we wait.
	if (pDevContext->StatusBlockBase != NULL) {
		UINT64 *sb = (UINT64 *)pDevContext->StatusBlockBase;
		DbgPrint("[DPC-SB] StatusBlock: [0]=0x%llx [1]=0x%llx [2]=0x%llx [3]=0x%llx\n",
			sb[0], sb[1], sb[2], sb[3]);
	}

	// FATAL_ERR detection (bit 12 = 0x1000).  Logged but does NOT block
	// completion — IOCTL path will surface the error via USER_HIB_ERROR.
	if (pDevContext->IsrSeenPendingBits & APEX_WIRE_BIT_FATAL_ERR) {
		DbgPrint("[DPC] FATAL_ERR observed in WIRE_INT_PENDING (bit 0x1000) — see USER_HIB_ERROR\n");
	}

	// PREMATURE-DPC GATE.
	//
	// Earlier version trusted SC_HOST_0 (bit 4) alone as "INFER done".  Bug:
	// SC_HOST_0 fires when the SCALAR core executes host_interrupt 0 — but on
	// observed traces this can fire BEFORE OUTFEED actually drains its writes
	// (we saw OUT_RUN_STATUS=0 at first ISR, OUTFEED=0x1 only 1ms later).
	// Releasing the OUTPUT mapping at that point caused the chip's later
	// outbound write to walk a freed/zeroed PTE → HIB_ERR=0x1 + FATAL_ERR
	// (the second ISR with pending=0x1011).
	//
	// New rule: complete only when ALL run-status engines are in a terminal
	// state.  Terminal = kIdle(0) (normal done) or kHalted(4) (error done).
	// This matches libedgetpu's IsAllRunStatusKIdle pattern and ensures
	// the OUTFEED engine has fully drained before we tear down the OUTPUT
	// extended mapping.
	//
	// FATAL_ERR seen in pending also forces completion (so we don't deadlock
	// on a dead chip).
	BOOLEAN scHostSeen = (pDevContext->IsrSeenPendingBits & APEX_WIRE_BIT_SC_HOST_0) != 0;
	BOOLEAN fatalSeen  = (pDevContext->IsrSeenPendingBits & APEX_WIRE_BIT_FATAL_ERR) != 0;
	UINT32  gateSc     = (bar2 != NULL) ? apex_read_register_32(bar2, APEX_REG_SCALAR_RUN_STATUS)     : 0xFFFFFFFFu;
	UINT32  gateIn     = (bar2 != NULL) ? apex_read_register_32(bar2, APEX_REG_INFEED_RUN_STATUS)     : 0xFFFFFFFFu;
	UINT32  gateOut    = (bar2 != NULL) ? apex_read_register_32(bar2, APEX_REG_OUTFEED_RUN_STATUS)    : 0xFFFFFFFFu;
	UINT32  gateAv     = (bar2 != NULL) ? apex_read_register_32(bar2, APEX_REG_AVDATA_POP_RUN_STATUS) : 0xFFFFFFFFu;
	BOOLEAN allIdle    = (gateSc == 0) && (gateOut == 0) && (gateIn == 0) && (gateAv == 0);
	BOOLEAN anyHalted  = (gateSc == 4) || (gateOut == 4);
	BOOLEAN terminal   = allIdle || anyHalted || fatalSeen;
	if (bar2 != NULL && !terminal) {
		DbgPrint("[DPC] PREMATURE: SC=0x%x IN=0x%x OUT=0x%x AV=0x%x scHost=%d fatal=%d — deferring (waiting for all-idle)\n",
			gateSc, gateIn, gateOut, gateAv, scHostSeen, fatalSeen);
		return; // keep MDLs locked, keep event unsignaled
	}
	if (terminal && !allIdle) {
		DbgPrint("[DPC] terminal-but-not-allIdle: SC=0x%x IN=0x%x OUT=0x%x AV=0x%x halted=%d fatal=%d scHost=%d\n",
			gateSc, gateIn, gateOut, gateAv, anyHalted, fatalSeen, scHostSeen);
	}

	if (pDevContext->InferOutputMdl != NULL && bar2 != NULL) {
		PMDL outMdl   = pDevContext->InferOutputMdl;
		UINT64 outVA   = pDevContext->InferOutputDeviceVA;
		UINT64 outSize = pDevContext->InferOutputSize;
		UINT32 pageCnt = (UINT32)((outSize + PAGE_SIZE - 1) >> PAGE_SHIFT);
		PPFN_NUMBER pfn = MmGetMdlPfnArray(outMdl);
		BOOLEAN isExtended = (outVA & (1ULL << 63)) != 0;

		// (a) PTE readback for first up-to-4 output pages.
		// Simple VA: read chip PTE[outVA>>12] directly, expect data PA.
		// Extended VA: read chip PTE[6144+((outVA>>21)&0x1FFF)], expect 2-level PT PA;
		//              also dump the host-resident sub-entries we wrote.
		if (!isExtended) {
			UINT32 pteIdx  = (UINT32)(outVA >> PAGE_SHIFT);
			UINT32 vc = (pageCnt < 4) ? pageCnt : 4;
			UINT32 ii;
			for (ii = 0; ii < vc; ii++) {
				UINT64 expectPA  = (UINT64)pfn[ii] << PAGE_SHIFT;
				UINT64 readPA    = apex_read_register(bar2, APEX_REG_PAGE_TABLE + ((pteIdx + ii) * 8));
				UINT64 readPaNoF = readPA & ~1ULL;
				DbgPrint("[DPC-PTE] OUTPUT PTE[%u+%u] expect=0x%llx read=0x%llx %s\n",
					pteIdx, ii, expectPA, readPaNoF,
					(readPaNoF == expectPA) ? "OK" : "MISMATCH");
			}
		} else {
			UINT32 subtableIdx    = (UINT32)((outVA >> 21) & 0x1FFF);
			UINT32 chipPteIdx     = 6144u + subtableIdx;
			UINT32 hostTableStart = (UINT32)((outVA >> 12) & 0x1FF);
			UINT64 chipReg = apex_read_register(bar2, APEX_REG_PAGE_TABLE + (chipPteIdx * 8));
			UINT64 expectPa = pDevContext->ExtPoolPa + ((UINT64)subtableIdx << PAGE_SHIFT);
			DbgPrint("[DPC-PTE] EXT chip PTE[%u] = 0x%llx (expected pool sub-region PA = 0x%llx | 0x1) %s\n",
				chipPteIdx, chipReg, expectPa,
				((chipReg & ~1ULL) == expectPa) ? "OK" : "MISMATCH");

			// Dump the host-resident 2-level PT entries we wrote.
			if (pDevContext->ExtPoolKva != NULL) {
				UINT64* slot = (UINT64*)((PUCHAR)pDevContext->ExtPoolKva +
				                         ((SIZE_T)subtableIdx << PAGE_SHIFT));
				UINT32 vc = (pageCnt < 4) ? pageCnt : 4;
				UINT32 ii;
				for (ii = 0; ii < vc; ii++) {
					UINT64 expectPA = (UINT64)pfn[ii] << PAGE_SHIFT;
					UINT64 entry = slot[hostTableStart + ii];
					UINT64 entryPa = entry & ~1ULL;
					DbgPrint("[DPC-PTE] EXT 2L[%u+%u] expect=0x%llx entry=0x%llx %s\n",
						hostTableStart, ii, expectPA, entry,
						(entryPa == expectPA) ? "OK" : "MISMATCH");
				}
			}
		}

		// (b) Map locked output pages into kernel VA, scan ENTIRE buffer for
		//     non-0xCC bytes (was 16 KB only — too small to catch chip writes
		//     that may land deeper into the buffer), dump first 64 bytes,
		//     and locate the first non-0xCC byte if any.
		{
			// In EXTENDED+BOUNCE mode the chip wrote into our < 4 GB bounce
			// buffer, not the user MDL.  Dump bounce in that case so we can
			// see whether OUTFEED actually wrote.  In SIMPLE mode chip wrote
			// directly into user buffer pages — dump the MDL kernel VA.
			PUCHAR kva = NULL;
			const char* tag = "USER";
			if (pDevContext->OutputBounceActive && pDevContext->OutputBounceKva != NULL) {
				kva = (PUCHAR)pDevContext->OutputBounceKva;
				tag = "BOUNCE";
			} else {
				kva = (PUCHAR)MmGetSystemAddressForMdlSafe(outMdl, NormalPagePriority);
			}
			if (kva != NULL) {
				ULONG dumpLen = (outSize < 64) ? (ULONG)outSize : 64;
				ULONG scanLen = (ULONG)outSize;   // full buffer scan
				ULONG j;
				ULONG nz = 0;
				ULONG ncc = 0;
				ULONG firstNonCc = 0xFFFFFFFF;
				for (j = 0; j < scanLen; j++) {
					if (kva[j] != 0) nz++;
					if (kva[j] != 0xCC) {
						ncc++;
						if (firstNonCc == 0xFFFFFFFF) firstNonCc = j;
					}
				}
				DbgPrint("[DPC-OUT/%s] kernel VA=%p outVA=0x%llx size=%llu nonzero=%lu non-0xCC=%lu (FULL scan) firstNon0xCC@0x%lx\n",
					tag, kva, outVA, outSize, nz, ncc, firstNonCc);
				if (firstNonCc != 0xFFFFFFFF && firstNonCc + 16 <= scanLen) {
					ULONG f = firstNonCc;
					DbgPrint("[DPC-OUT/%s] firstNon0xCC@0x%lx: %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
						tag, f,
						kva[f+0], kva[f+1], kva[f+2],  kva[f+3],
						kva[f+4], kva[f+5], kva[f+6],  kva[f+7],
						kva[f+8], kva[f+9], kva[f+10], kva[f+11],
						kva[f+12],kva[f+13],kva[f+14], kva[f+15]);
				}
				// Also dump tail 64 B — chip may write near end of buffer.
				if (scanLen >= 64) {
					ULONG t = scanLen - 64;
					DbgPrint("[DPC-OUT/%s] tail64@0x%lx: %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
						tag, t,
						kva[t+0], kva[t+1], kva[t+2],  kva[t+3],
						kva[t+4], kva[t+5], kva[t+6],  kva[t+7],
						kva[t+8], kva[t+9], kva[t+10], kva[t+11],
						kva[t+12],kva[t+13],kva[t+14], kva[t+15]);
				}
				DbgPrint("[DPC-OUT] [00] %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
					kva[0],  kva[1],  kva[2],  kva[3],  kva[4],  kva[5],  kva[6],  kva[7],
					kva[8],  kva[9],  kva[10], kva[11], kva[12], kva[13], kva[14], kva[15]);
				if (dumpLen > 16) {
					DbgPrint("[DPC-OUT] [10] %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
						kva[16], kva[17], kva[18], kva[19], kva[20], kva[21], kva[22], kva[23],
						kva[24], kva[25], kva[26], kva[27], kva[28], kva[29], kva[30], kva[31]);
				}
				if (dumpLen > 32) {
					DbgPrint("[DPC-OUT] [20] %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
						kva[32], kva[33], kva[34], kva[35], kva[36], kva[37], kva[38], kva[39],
						kva[40], kva[41], kva[42], kva[43], kva[44], kva[45], kva[46], kva[47]);
				}
				if (dumpLen > 48) {
					DbgPrint("[DPC-OUT] [30] %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
						kva[48], kva[49], kva[50], kva[51], kva[52], kva[53], kva[54], kva[55],
						kva[56], kva[57], kva[58], kva[59], kva[60], kva[61], kva[62], kva[63]);
				}
			} else {
				DbgPrint("[DPC-OUT] MmGetSystemAddressForMdlSafe returned NULL — cannot dump output bytes\n");
			}
		}
	}

	// =========================================================
	// DPC-SCAN — chip 이 OUTFEED 로 발행한 W beat 가 host 의 어느 영역에 도달했는지 추적.
	// MDL 을 unlock 하기 직전이라 모든 host VA 가 살아있음.  OUTPUT 이 0xCC 그대로 남아도
	// 여기서 다른 영역에 stray write 흔적이 있다면 chip 의 진짜 destination 을 역추적할 수 있음.
	// =========================================================
	{
		// 1) DescRing — pre-INFER 에 0xA5 fill, descriptor 32 byte 만 우리가 씀.
		if (pDevContext->DescRingBase != NULL) {
			PUCHAR p = (PUCHAR)pDevContext->DescRingBase;
			ULONG nonA5 = 0;
			ULONG firstNonA5 = 0xFFFFFFFF;
			ULONG ii;
			for (ii = 32; ii < PAGE_SIZE; ii++) {
				if (p[ii] != 0xA5) {
					nonA5++;
					if (firstNonA5 == 0xFFFFFFFF) firstNonA5 = ii;
				}
			}
			DbgPrint("[DPC-SCAN-DescRing] post-32 non-0xA5=%lu firstAt=0x%lx\n",
				nonA5, firstNonA5);
			if (nonA5 > 0 && firstNonA5 != 0xFFFFFFFF) {
				ULONG d = firstNonA5;
				DbgPrint("[DPC-SCAN-DescRing] @0x%lx: %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
					d,
					p[d+0],p[d+1],p[d+2],p[d+3], p[d+4],p[d+5],p[d+6],p[d+7],
					p[d+8],p[d+9],p[d+10],p[d+11], p[d+12],p[d+13],p[d+14],p[d+15]);
			}
		}

		// 2) StatusBlock — pre-INFER 에 0xA5 fill, chip 이 head 16 byte 에 IQ_completed_head 만 씀.
		if (pDevContext->StatusBlockBase != NULL) {
			PUCHAR p = (PUCHAR)pDevContext->StatusBlockBase;
			ULONG nonA5 = 0;
			ULONG firstNonA5 = 0xFFFFFFFF;
			ULONG ii;
			for (ii = 16; ii < PAGE_SIZE; ii++) {
				if (p[ii] != 0xA5) {
					nonA5++;
					if (firstNonA5 == 0xFFFFFFFF) firstNonA5 = ii;
				}
			}
			DbgPrint("[DPC-SCAN-StatusBlock] post-16 non-0xA5=%lu firstAt=0x%lx\n",
				nonA5, firstNonA5);
			if (nonA5 > 0 && firstNonA5 != 0xFFFFFFFF) {
				ULONG d = firstNonA5;
				DbgPrint("[DPC-SCAN-StatusBlock] @0x%lx: %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
					d,
					p[d+0],p[d+1],p[d+2],p[d+3], p[d+4],p[d+5],p[d+6],p[d+7],
					p[d+8],p[d+9],p[d+10],p[d+11], p[d+12],p[d+13],p[d+14],p[d+15]);
			}
		}

		// 3) INFER bitstream MDL — chip 이 자기 instruction 영역을 덮어쓰면 head 가 변함.
		//    pre-INFER 의 head bytes 는 80 0f 00 28 c7 00 00 00 ...
		if (pDevContext->LockedModelMdl != NULL) {
			PUCHAR kva = (PUCHAR)MmGetSystemAddressForMdlSafe(
				pDevContext->LockedModelMdl, NormalPagePriority);
			if (kva != NULL) {
				SIZE_T sz = MmGetMdlByteCount(pDevContext->LockedModelMdl);
				DbgPrint("[DPC-SCAN-INFERBitstream] sz=%llu first16=%02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x | tail16=%02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
					(UINT64)sz,
					kva[0],kva[1],kva[2],kva[3], kva[4],kva[5],kva[6],kva[7],
					kva[8],kva[9],kva[10],kva[11], kva[12],kva[13],kva[14],kva[15],
					kva[sz-16],kva[sz-15],kva[sz-14],kva[sz-13], kva[sz-12],kva[sz-11],kva[sz-10],kva[sz-9],
					kva[sz-8],kva[sz-7],kva[sz-6],kva[sz-5], kva[sz-4],kva[sz-3],kva[sz-2],kva[sz-1]);
			}
		}

		// 4) Input image — chip 이 OUTPUT 의 VA 를 잘못 디코드해서 INPUT 영역에
		//    write back 했을 가능성.  pre-INFER 의 head 는 image pixel pattern.
		if (pDevContext->InferInputMdl != NULL) {
			PUCHAR kva = (PUCHAR)MmGetSystemAddressForMdlSafe(
				pDevContext->InferInputMdl, NormalPagePriority);
			if (kva != NULL) {
				SIZE_T sz = MmGetMdlByteCount(pDevContext->InferInputMdl);
				DbgPrint("[DPC-SCAN-InputImage] sz=%llu first16=%02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x | tail16=%02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
					(UINT64)sz,
					kva[0],kva[1],kva[2],kva[3], kva[4],kva[5],kva[6],kva[7],
					kva[8],kva[9],kva[10],kva[11], kva[12],kva[13],kva[14],kva[15],
					kva[sz-16],kva[sz-15],kva[sz-14],kva[sz-13], kva[sz-12],kva[sz-11],kva[sz-10],kva[sz-9],
					kva[sz-8],kva[sz-7],kva[sz-6],kva[sz-5], kva[sz-4],kva[sz-3],kva[sz-2],kva[sz-1]);
			}
		}

		// 5) Cached PARAM bitstream — chip 이 instruction 영역을 자기 결과로 덮을 수 있음.
		if (pDevContext->CachedParamBitstreamMdl != NULL) {
			PUCHAR kva = (PUCHAR)MmGetSystemAddressForMdlSafe(
				pDevContext->CachedParamBitstreamMdl, NormalPagePriority);
			if (kva != NULL) {
				SIZE_T sz = MmGetMdlByteCount(pDevContext->CachedParamBitstreamMdl);
				DbgPrint("[DPC-SCAN-PARAMBitstream] sz=%llu first16=%02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x | tail16=%02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
					(UINT64)sz,
					kva[0],kva[1],kva[2],kva[3], kva[4],kva[5],kva[6],kva[7],
					kva[8],kva[9],kva[10],kva[11], kva[12],kva[13],kva[14],kva[15],
					kva[sz-16],kva[sz-15],kva[sz-14],kva[sz-13], kva[sz-12],kva[sz-11],kva[sz-10],kva[sz-9],
					kva[sz-8],kva[sz-7],kva[sz-6],kva[sz-5], kva[sz-4],kva[sz-3],kva[sz-2],kva[sz-1]);
			}
		}
	}

	if (pDevContext->InferInputMdl != NULL) {
		MmUnlockPages(pDevContext->InferInputMdl);
		IoFreeMdl(pDevContext->InferInputMdl);
		pDevContext->InferInputMdl = NULL;
		DbgPrint("[%s] Input MDL unlocked\n", __FUNCTION__);
	}

	if (pDevContext->InferOutputMdl != NULL) {
		// EXTENDED+BOUNCE path: chip wrote into our < 4 GB bounce buffer.
		// Copy bounce -> user buffer (via MDL kernel VA, MDL still locked
		// here) BEFORE unlocking, while user pages are guaranteed resident.
		if (pDevContext->OutputBounceActive && pDevContext->OutputBounceKva != NULL) {
			PUCHAR userKva = (PUCHAR)MmGetSystemAddressForMdlSafe(
				pDevContext->InferOutputMdl, NormalPagePriority);
			if (userKva != NULL) {
				SIZE_T copySize = pDevContext->InferOutputSize;
				if (copySize > pDevContext->OutputBounceSize) {
					copySize = pDevContext->OutputBounceSize;
				}
				RtlCopyMemory(userKva, pDevContext->OutputBounceKva, copySize);
				DbgPrint("[%s] Bounce copied to user buffer (size=%llu)\n",
					__FUNCTION__, (UINT64)copySize);
				DbgPrint("[%s] BOUNCE first 16B: %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
					__FUNCTION__,
					((PUCHAR)pDevContext->OutputBounceKva)[0],
					((PUCHAR)pDevContext->OutputBounceKva)[1],
					((PUCHAR)pDevContext->OutputBounceKva)[2],
					((PUCHAR)pDevContext->OutputBounceKva)[3],
					((PUCHAR)pDevContext->OutputBounceKva)[4],
					((PUCHAR)pDevContext->OutputBounceKva)[5],
					((PUCHAR)pDevContext->OutputBounceKva)[6],
					((PUCHAR)pDevContext->OutputBounceKva)[7],
					((PUCHAR)pDevContext->OutputBounceKva)[8],
					((PUCHAR)pDevContext->OutputBounceKva)[9],
					((PUCHAR)pDevContext->OutputBounceKva)[10],
					((PUCHAR)pDevContext->OutputBounceKva)[11],
					((PUCHAR)pDevContext->OutputBounceKva)[12],
					((PUCHAR)pDevContext->OutputBounceKva)[13],
					((PUCHAR)pDevContext->OutputBounceKva)[14],
					((PUCHAR)pDevContext->OutputBounceKva)[15]);
			} else {
				DbgPrint("[%s] WARNING: cannot map output MDL — bounce data lost\n",
					__FUNCTION__);
			}
		}
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
// Lightweight PCI Command/Status dump — callable from any IRQL <= DISPATCH_LEVEL.
// We use this to prove (or disprove) the "chip can't issue outbound writes" hypothesis:
//   Command bit2 (BME) must be 1 for the device to act as a PCIe bus master.
//   Status bit13 (Received Master Abort) signals the root complex rejected a write —
//     usually IOMMU/VT-d denying the PA the chip targeted.
VOID npudriverDumpPciCommand(WDFDEVICE Device, const char* tag)
{
	BUS_INTERFACE_STANDARD bus = { 0 };
	NTSTATUS status;
	UINT16  cmd = 0;
	UINT16  st  = 0;
	ULONG   bytes;

	status = WdfFdoQueryForInterface(
		Device,
		&GUID_BUS_INTERFACE_STANDARD,
		(PINTERFACE)&bus,
		sizeof(BUS_INTERFACE_STANDARD),
		1, NULL);
	if (!NT_SUCCESS(status)) {
		DbgPrint("[PCI-CMD@%s] QueryForInterface failed 0x%x\n", tag, status);
		return;
	}

	bytes = bus.GetBusData(bus.Context, PCI_WHICHSPACE_CONFIG, &cmd, 0x04, 2);
	if (bytes != 2) {
		DbgPrint("[PCI-CMD@%s] read Command failed (got %lu)\n", tag, bytes);
		bus.InterfaceDereference(bus.Context);
		return;
	}
	bytes = bus.GetBusData(bus.Context, PCI_WHICHSPACE_CONFIG, &st, 0x06, 2);
	if (bytes != 2) {
		DbgPrint("[PCI-CMD@%s] read Status failed (got %lu)\n", tag, bytes);
		bus.InterfaceDereference(bus.Context);
		return;
	}

	DbgPrint("[PCI-CMD@%s] Command=0x%04x  BME=%u MEM=%u IO=%u  | Status=0x%04x  RcvMasterAbort=%u SigTargetAbort=%u SigSysErr=%u DataParityErr=%u\n",
		tag,
		cmd, (cmd >> 2) & 1, (cmd >> 1) & 1, cmd & 1,
		st,  (st  >> 13) & 1, (st  >> 11) & 1, (st >> 14) & 1, (st >> 8) & 1);

	// Best-effort: clear sticky error bits (W1C) so subsequent snapshots show only
	// new errors that occurred since last clear.  Errors at startup are normal.
	if (st & 0xF900) {
		UINT16 w1c = (UINT16)(st & 0xF900);
		bus.SetBusData(bus.Context, PCI_WHICHSPACE_CONFIG, &w1c, 0x06, 2);
		DbgPrint("[PCI-CMD@%s] cleared sticky Status bits 0x%04x\n", tag, w1c);
	}

	bus.InterfaceDereference(bus.Context);
}

// PCIe Express Capability + AER dump.  Tells us whether chip's AXI activity
// reaches the PCIe link by checking PCIe Device Status error bits and AER
// Uncorrectable/Correctable error status registers.
VOID npudriverDumpPciAer(WDFDEVICE Device, const char* tag)
{
	BUS_INTERFACE_STANDARD bus = { 0 };
	NTSTATUS status;
	UCHAR  pcieCap[24];
	UCHAR  extCfg[256];
	ULONG  bytes;
	UINT32 aerOff = 0xFFFFFFFFu;

	status = WdfFdoQueryForInterface(
		Device, &GUID_BUS_INTERFACE_STANDARD,
		(PINTERFACE)&bus, sizeof(BUS_INTERFACE_STANDARD), 1, NULL);
	if (!NT_SUCCESS(status)) {
		DbgPrint("[PCIE@%s] QueryForInterface failed 0x%x\n", tag, status);
		return;
	}

	// PCIe Express Capability lives at PCI config 0x80 (id=0x10, seen in MSIX-CAP log).
	// Layout: +0 cap header, +0x08 DevCtrl, +0x0a DevStatus, +0x10 LinkCtrl, +0x12 LinkStatus
	bytes = bus.GetBusData(bus.Context, PCI_WHICHSPACE_CONFIG, pcieCap, 0x80, 24);
	if (bytes >= 24) {
		UINT16 devCtrl    = *(UINT16*)(pcieCap + 0x08);
		UINT16 devStatus  = *(UINT16*)(pcieCap + 0x0a);
		UINT16 linkCtrl   = *(UINT16*)(pcieCap + 0x10);
		UINT16 linkStatus = *(UINT16*)(pcieCap + 0x12);
		DbgPrint("[PCIE@%s] DevCtrl=0x%04x DevStatus=0x%04x  CorrErr=%u NonFatalErr=%u FatalErr=%u UnsupReqDetected=%u  | LinkCtrl=0x%04x LinkStatus=0x%04x  speed=%u width=%u\n",
			tag, devCtrl, devStatus,
			devStatus & 1, (devStatus >> 1) & 1, (devStatus >> 2) & 1, (devStatus >> 3) & 1,
			linkCtrl, linkStatus,
			linkStatus & 0xF, (linkStatus >> 4) & 0x3F);

		// Clear DevStatus error bits (W1C) so next snapshot only shows new errors.
		if (devStatus & 0x000F) {
			UINT16 w1c = devStatus & 0x000F;
			bus.SetBusData(bus.Context, PCI_WHICHSPACE_CONFIG, &w1c, 0x80 + 0x0a, 2);
			DbgPrint("[PCIE@%s] cleared sticky DevStatus error bits 0x%04x\n", tag, w1c);
		}
	} else {
		DbgPrint("[PCIE@%s] PCIe cap read failed (got %lu bytes)\n", tag, bytes);
	}

	// PCIe extended config space (0x100..0x1FF) — AER is here.
	// Walk extended capability chain looking for AER (cap ID = 0x0001).
	bytes = bus.GetBusData(bus.Context, PCI_WHICHSPACE_CONFIG, extCfg, 0x100, 256);
	if (bytes >= 64) {
		UINT32 nextOff = 0x100;
		int    safety = 16;
		while (nextOff != 0 && nextOff >= 0x100 && nextOff < 0x200 && safety-- > 0) {
			UINT32 idx = nextOff - 0x100;
			UINT32 hdr = *(UINT32*)(extCfg + idx);
			UINT16 id  = (UINT16)(hdr & 0xFFFF);
			UINT16 ver = (UINT16)((hdr >> 16) & 0xF);
			UINT16 nxt = (UINT16)((hdr >> 20) & 0xFFF);
			DbgPrint("[PCIE-EXT@%s] @0x%03x id=0x%04x ver=%u next=0x%03x\n",
				tag, nextOff, id, ver, nxt);
			if (id == 0x0001) {
				aerOff = idx;
				break;
			}
			if (nxt == 0 || nxt < 0x100) break;
			nextOff = nxt;
		}

		if (aerOff != 0xFFFFFFFFu) {
			UINT32 uncorStatus = *(UINT32*)(extCfg + aerOff + 0x04);
			UINT32 uncorMask   = *(UINT32*)(extCfg + aerOff + 0x08);
			UINT32 uncorSev    = *(UINT32*)(extCfg + aerOff + 0x0C);
			UINT32 corStatus   = *(UINT32*)(extCfg + aerOff + 0x10);
			UINT32 corMask     = *(UINT32*)(extCfg + aerOff + 0x14);
			UINT32 ctrl        = *(UINT32*)(extCfg + aerOff + 0x18);
			DbgPrint("[AER@%s] uncorStatus=0x%08x uncorMask=0x%08x uncorSev=0x%08x | corStatus=0x%08x corMask=0x%08x | ctrl=0x%08x\n",
				tag, uncorStatus, uncorMask, uncorSev, corStatus, corMask, ctrl);
			// Decoded uncor bits we care about for outbound writes:
			//   bit12 = Poisoned TLP Received
			//   bit13 = Flow Control Protocol Error
			//   bit14 = Completion Timeout
			//   bit15 = Completer Abort
			//   bit16 = Unexpected Completion
			//   bit18 = Malformed TLP
			//   bit20 = Unsupported Request
			DbgPrint("[AER@%s]   uncor: PoisonedTLP=%u FlowCtrl=%u CompletionTimeout=%u CompleterAbort=%u UnexpComp=%u MalformedTLP=%u UnsupReq=%u\n",
				tag,
				(uncorStatus >> 12) & 1, (uncorStatus >> 13) & 1, (uncorStatus >> 14) & 1,
				(uncorStatus >> 15) & 1, (uncorStatus >> 16) & 1, (uncorStatus >> 18) & 1,
				(uncorStatus >> 20) & 1);
			DbgPrint("[AER@%s]   cor:   ReceiverErr=%u BadTLP=%u BadDLLP=%u Replay=%u\n",
				tag,
				corStatus & 1, (corStatus >> 6) & 1, (corStatus >> 7) & 1, (corStatus >> 8) & 1);

			// Clear AER status (W1C)
			if (uncorStatus) {
				UINT32 w1c = uncorStatus;
				bus.SetBusData(bus.Context, PCI_WHICHSPACE_CONFIG, &w1c, 0x100 + aerOff + 0x04, 4);
			}
			if (corStatus) {
				UINT32 w1c = corStatus;
				bus.SetBusData(bus.Context, PCI_WHICHSPACE_CONFIG, &w1c, 0x100 + aerOff + 0x10, 4);
			}
		} else {
			DbgPrint("[AER@%s] AER capability not found in extended config\n", tag);
			// Dump first 16 bytes so we know what's there
			DbgPrint("[PCIE-EXT@%s] @0x100 raw: %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
				tag,
				extCfg[0],extCfg[1],extCfg[2],extCfg[3], extCfg[4],extCfg[5],extCfg[6],extCfg[7],
				extCfg[8],extCfg[9],extCfg[10],extCfg[11], extCfg[12],extCfg[13],extCfg[14],extCfg[15]);
		}
	} else {
		DbgPrint("[PCIE-EXT@%s] extended config read returned %lu bytes (need 64+)\n", tag, bytes);
	}

	bus.InterfaceDereference(bus.Context);
}

// Mini CSR sweep — read ONLY the named registers we already use safely
// elsewhere in the driver.  A blind sweep over all of BAR2 hangs because some
// offsets do not return PCIe completions, freezing the CPU on the MMIO read.
// `tag` distinguishes pre/post snapshots in the log.  Caller dumps once before
// inference and once after; user diffs the two by eye / grep.
VOID npudriverDumpKnownCsrs(PDEVICE_CONTEXT ctx, const char* tag)
{
	PVOID b2 = ctx->Bar2BaseAddress;
	if (b2 == NULL) return;

	#define R32(name, off) DbgPrint("[CSR@%s] %-32s @0x%05x = 0x%08x\n", \
		tag, name, (off), apex_read_register_32(b2, (off)))

	// MMU / page-table
	R32("PAGE_TABLE_SIZE",             0x46000);
	R32("EXTENDED_TABLE",              0x46008);
	R32("KERNEL_HIB_TRANSLATION_EN",   0x46010);

	// HIB credits (input/param/instr/output)
	R32("HIB_INSTRUCTION_CREDITS",     APEX_REG_HIB_INSTRUCTION_CREDITS);
	R32("HIB_INPUT_ACTV_CREDITS",      APEX_REG_HIB_INPUT_ACTV_CREDITS);
	R32("HIB_PARAM_CREDITS",           APEX_REG_HIB_PARAM_CREDITS);
	R32("HIB_OUTPUT_ACTV_CREDITS",     APEX_REG_HIB_OUTPUT_ACTV_CREDITS);

	// AXI shim counters (the ones that moved in the 0x1FFA000 redirect run)
	R32("AXI_AW_CREDIT_INSERTION",     APEX_REG_AXI_AW_CREDIT_SHIM_INSERTION);
	R32("AXI_W_CREDIT_INSERTION",      APEX_REG_AXI_W_CREDIT_SHIM_INSERTION);
	R32("AXI_AW_CREDIT_OCCUPANCY",     APEX_REG_AXI_AW_CREDIT_SHIM_OCCUPANCY);
	R32("AXI_W_CREDIT_OCCUPANCY",      APEX_REG_AXI_W_CREDIT_SHIM_OCCUPANCY);

	// Engine run-status
	R32("SCALAR_RUN_STATUS",           APEX_REG_SCALAR_RUN_STATUS);
	R32("INFEED_RUN_STATUS",           APEX_REG_INFEED_RUN_STATUS);
	R32("OUTFEED_RUN_STATUS",          APEX_REG_OUTFEED_RUN_STATUS);
	R32("PARAMETER_POP_RUN_STATUS",    APEX_REG_PARAMETER_POP_RUN_STATUS);
	R32("AVDATA_POP_RUN_STATUS",       APEX_REG_AVDATA_POP_RUN_STATUS);

	// IQ status / int / wire
	R32("INSTR_QUEUE_STATUS",          APEX_REG_INSTR_QUEUE_STATUS);
	R32("INSTR_QUEUE_CONTROL",         APEX_REG_INSTR_QUEUE_CONTROL);
	R32("INSTR_QUEUE_INT_STATUS",      APEX_REG_INSTR_QUEUE_INT_STATUS);
	R32("WIRE_INT_PENDING",            APEX_REG_WIRE_INT_PENDING);
	R32("WIRE_INT_MASK",               APEX_REG_WIRE_INT_MASK);

	// Errors
	R32("USER_HIB_ERROR_STATUS",       APEX_REG_USER_HIB_ERROR_STATUS);
	R32("SCALAR_CORE_ERROR_STATUS",    APEX_REG_SCALAR_CORE_ERROR_STATUS);

	#undef R32
}

NTSTATUS npudriverDumpMsixCapability(WDFDEVICE Device)
{
	BUS_INTERFACE_STANDARD bus = { 0 };
	NTSTATUS status;
	UCHAR hdr[64];
	UCHAR cap[16];
	UCHAR capPtr;
	int safety;
	UINT16 vendorId, deviceId, statusReg, cmdReg;
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
	cmdReg    = *(UINT16*)(hdr + 0x04);
	deviceId  = *(UINT16*)(hdr + 0x02);
	statusReg = *(UINT16*)(hdr + 0x06);
	capPtr    = hdr[0x34];

	DbgPrint("[MSIX-CAP] PCI VID=0x%04x DID=0x%04x Cmd=0x%04x Status=0x%04x CapPtr=0x%02x\n",
		vendorId, deviceId, cmdReg, statusReg, capPtr);
	DbgPrint("[MSIX-CAP]   Cmd bits: BME=%u MEM=%u IO=%u  Status bits: RcvMasterAbort=%u SigTargetAbort=%u\n",
		(cmdReg >> 2) & 1, (cmdReg >> 1) & 1, cmdReg & 1,
		(statusReg >> 13) & 1, (statusReg >> 11) & 1);

	// CRITICAL: if BME is 0 here, chip CANNOT emit outbound writes.  Force it on.
	// Windows normally sets BME during PCI enumeration but if for any reason it
	// got cleared (D3 transition, driver reload, etc.) we must restore it.
	if (((cmdReg >> 2) & 1) == 0) {
		UINT16 newCmd = cmdReg | 0x0006;  // set BME(2) | MEM(1)
		DbgPrint("[MSIX-CAP] BME=0 detected — forcing Command 0x%04x -> 0x%04x\n", cmdReg, newCmd);
		bus.SetBusData(bus.Context, PCI_WHICHSPACE_CONFIG, &newCmd, 0x04, 2);
		bus.GetBusData(bus.Context, PCI_WHICHSPACE_CONFIG, &cmdReg, 0x04, 2);
		DbgPrint("[MSIX-CAP] readback Cmd=0x%04x BME=%u\n", cmdReg, (cmdReg >> 2) & 1);
	}

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

