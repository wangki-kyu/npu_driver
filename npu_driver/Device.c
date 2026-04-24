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

