#include "Driver.h"
#include "Memory.h"
#include "Queue.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, npudriverCreateDevice)
#pragma alloc_text(PAGE, npudriverEvtDevicePrepareHardware)
#pragma alloc_text(PAGE, npudriverEvtDeviceReleaseHardware)
#endif

NTSTATUS npudriverSettingResourceInfo(WDFDEVICE Device, WDFCMRESLIST ResourceList);
VOID npudriverReadTemperature(WDFDEVICE Device);

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

		// Initialize inference MDLs
		deviceContext->InferInputMdl = NULL;
		deviceContext->InferOutputMdl = NULL;

		// Initialize descriptor ring fields
		deviceContext->DescRingBase = NULL;
		deviceContext->DescRingDeviceVA = 0;
		deviceContext->DescRingTail = 0;
		deviceContext->StatusBlockBase = NULL;
		deviceContext->StatusBlockDeviceVA = 0;

		// Initialize inference completion event
		KeInitializeEvent(&deviceContext->InferCompleteEvent, NotificationEvent, FALSE);

		// Create interrupt handler
		{
			WDF_INTERRUPT_CONFIG interruptConfig;
			WDF_INTERRUPT_CONFIG_INIT(&interruptConfig,
				npudriverEvtInterruptIsr,
				npudriverEvtInterruptDpc);
			status = WdfInterruptCreate(device, &interruptConfig,
				WDF_NO_OBJECT_ATTRIBUTES, &deviceContext->Interrupts[0]);
			if (!NT_SUCCESS(status)) {
				DbgPrint("[%s] WdfInterruptCreate failed: 0x%x\n", __FUNCTION__, status);
				return status;
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

	// Initialize page table for memory mapping
	status = ApexPageTableInit(Device);
	if (!NT_SUCCESS(status)) {
		DbgPrint("[%s] ApexPageTableInit failed: 0x%x\n", __FUNCTION__, status);
		return status;
	}

	PDEVICE_CONTEXT deviceContext = DeviceGetContext(Device);

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

	DbgPrint("[%s] Starting GCB quit-reset sequence\n", __FUNCTION__);
	apex_rmw_register_32(deviceContext->Bar2BaseAddress, APEX_REG_SCU_3, 0x0, 2, 14); // Disable RAM shutdown
	apex_rmw_register_32(deviceContext->Bar2BaseAddress, APEX_REG_SCU_2, 0x0, 2, 18); // Disable clock gate
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

	// rg_pwr_state_ovr SCU_3[27:26] = 0b10 (active, no clock gating override)
	apex_rmw_register_32(deviceContext->Bar2BaseAddress, APEX_REG_SCU_3, 0x2, 2, 26);
	{
		UINT32 scu3After = apex_read_register_32(deviceContext->Bar2BaseAddress, APEX_REG_SCU_3);
		DbgPrint("[%s] SCU_3 after power-state override: 0x%08x\n", __FUNCTION__, scu3After);
	}

	// Clear AXI quiesce — MUST happen after quit-reset, before run controls.
	// If left set, scalar core cannot push DMA descriptors to INFEED/OUTFEED via
	// internal AXI bus: engines do exactly ONE DMA (the pre-quiesce one) then halt at 0x4.
	{
		UINT32 aqBefore = apex_read_register_32(deviceContext->Bar2BaseAddress, APEX_REG_AXI_QUIESCE);
		DbgPrint("[%s] AXI_QUIESCE before clear: 0x%08x\n", __FUNCTION__, aqBefore);
		apex_write_register_32(deviceContext->Bar2BaseAddress, APEX_REG_AXI_QUIESCE, 0);
		UINT32 aqAfter = apex_read_register_32(deviceContext->Bar2BaseAddress, APEX_REG_AXI_QUIESCE);
		DbgPrint("[%s] AXI_QUIESCE after clear: 0x%08x\n", __FUNCTION__, aqAfter);
	}

	// Unpause DMA engines — the pause written before reset must be explicitly cleared.
	// GCB reset may or may not clear this register; write 0 unconditionally.
	apex_write_register(deviceContext->Bar2BaseAddress, APEX_REG_USER_HIB_DMA_PAUSE, 0);
	DbgPrint("[%s] USER_HIB_DMA_PAUSE cleared (DMA unpaused)\n", __FUNCTION__);

	DbgPrint("[%s] GCB reset/quit-reset sequence complete\n", __FUNCTION__);

	// ExitReset step 4 (libedgetpu beagle_top_level_handler.cc::QuitReset):
	// Re-enable idle register after reset. IdleRegister bit[31]=disable_idle (0=on),
	// bits[30:0]=counter. set_enable()+set_counter(1) → 0x1.
	{
		PVOID bar2 = deviceContext->Bar2BaseAddress;
		apex_write_register(bar2, APEX_REG_IDLEGENERATOR, 0x00000001ULL);
		DbgPrint("[%s] IdleRegister → 0x1 (enabled, counter=1)\n", __FUNCTION__);

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

		// SC_HOST_INT_CONTROL: enable the SC_HOST completion interrupt source
		apex_write_register(deviceContext->Bar2BaseAddress,
						   APEX_REG_SC_HOST_INT_CONTROL, 1);
		DbgPrint("[%s] SC_HOST_INT_CONTROL → 1 (interrupt enabled)\n", __FUNCTION__);

		// INSTR_QUEUE_INTVECCTL: route instruction queue interrupt to MSI-X vector 0
		apex_write_register(deviceContext->Bar2BaseAddress,
						   APEX_REG_INSTR_QUEUE_INTVECCTL, 0);
		DbgPrint("[%s] INSTR_QUEUE_INTVECCTL → MSI-X vector 0\n", __FUNCTION__);

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

		// Disable breakpoints — default after GCB reset is 0 = "halt at PC=0".
		apex_write_register(bar2, APEX_REG_SCALAR_BREAKPOINT,        0xFFFFFFFFFFFFFFFFULL);
		apex_write_register(bar2, APEX_REG_INFEED_BREAKPOINT,        0xFFFFFFFFFFFFFFFFULL);
		apex_write_register(bar2, APEX_REG_OUTFEED_BREAKPOINT,       0xFFFFFFFFFFFFFFFFULL);
		apex_write_register(bar2, APEX_REG_PARAMETER_POP_BREAKPOINT, 0xFFFFFFFFFFFFFFFFULL);
		apex_write_register(bar2, APEX_REG_AVDATA_POP_BREAKPOINT,    0xFFFFFFFFFFFFFFFFULL);
		apex_write_register(bar2, APEX_REG_TILE_OP_BREAKPOINT,       0xFFFFFFFFFFFFFFFFULL);
		DbgPrint("[%s] Breakpoints disabled\n", __FUNCTION__);

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
		DbgPrint("[RB] IDLEGENERATOR       = 0x%llx  (기대 0x1)\n",
			apex_read_register(b, APEX_REG_IDLEGENERATOR));
		DbgPrint("[RB] TILE_DEEP_SLEEP     = 0x%llx  (기대 0x1e02)\n",
			apex_read_register(b, APEX_REG_TILE_DEEP_SLEEP));

		// breakpoint 확인 (0이면 VA=0에서 halt됨)
		DbgPrint("[RB] SCALAR_BREAKPOINT   = 0x%llx  (기대 0xffffffffffffffff)\n",
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

		DbgPrint("[%s] Resource[%lu]: Type=%lu", __FUNCTION__, i, descriptor->Type);

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
	UNREFERENCED_PARAMETER(MessageID);

	WDFDEVICE device = WdfInterruptGetDevice(Interrupt);
	PDEVICE_CONTEXT pDevContext = DeviceGetContext(device);

	if (pDevContext->Bar2BaseAddress == NULL) {
		return FALSE;
	}

	UINT64 pending = apex_read_register(pDevContext->Bar2BaseAddress,
									   APEX_REG_WIRE_INT_PENDING);
	if (pending == 0) {
		return FALSE;
	}

	UINT64 iqIntStatus = apex_read_register(pDevContext->Bar2BaseAddress,
											APEX_REG_INSTR_QUEUE_INT_STATUS);
	DbgPrint("[%s] Interrupt pending: 0x%llx iq_int_status=0x%llx\n",
			 __FUNCTION__, pending, iqIntStatus);

	apex_write_register(pDevContext->Bar2BaseAddress,
					   APEX_REG_WIRE_INT_PENDING, pending);

	WdfInterruptQueueDpcForIsr(Interrupt);
	return TRUE;
}

VOID npudriverEvtInterruptDpc(WDFINTERRUPT Interrupt, WDFOBJECT AssociatedObject)
{
	UNREFERENCED_PARAMETER(AssociatedObject);

	WDFDEVICE device = WdfInterruptGetDevice(Interrupt);
	PDEVICE_CONTEXT pDevContext = DeviceGetContext(device);

	DbgPrint("[%s] DPC: Inference complete, unlocking pages\n", __FUNCTION__);

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

