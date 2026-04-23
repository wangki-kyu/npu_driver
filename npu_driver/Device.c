#include "Driver.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, npudriverCreateDevice)
#pragma alloc_text(PAGE, npudriverEvtDevicePrepareHardware)
#pragma alloc_text(PAGE, npudriverEvtDeviceReleaseHardware)
#endif

NTSTATUS npudriverCreateDevice(PWDFDEVICE_INIT DeviceInit)
{
	WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
	WDF_OBJECT_ATTRIBUTES deviceAttributes;
	PDEVICE_CONTEXT deviceContext;
	WDFDEVICE device;
	NTSTATUS status;

	PAGED_CODE();

	WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
	pnpPowerCallbacks.EvtDevicePrepareHardware = npudriverEvtDevicePrepareHardware;
	pnpPowerCallbacks.EvtDeviceReleaseHardware= npudriverEvtDeviceReleaseHardware;

	WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);

	status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);

	if (NT_SUCCESS(status))
	{
		deviceContext = DeviceGetContext(device);

		// Initialize the context
		deviceContext->PrivateDeviceData = 0;

		// Create a device interface so that application can find and talk to us 

		status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_npudriver, NULL);

		if (NT_SUCCESS(status))
		{
			// todo: Initialize the I/O Package and any Queues

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
	UNREFERENCED_PARAMETER(ResourceList);
	UNREFERENCED_PARAMETER(ResourceListTranslated);

	return STATUS_SUCCESS;
}

NTSTATUS
npudriverEvtDeviceReleaseHardware(
	_In_ WDFDEVICE Device,
	_In_ WDFCMRESLIST ResourcesTranslated
)
{
	UNREFERENCED_PARAMETER(Device);
	UNREFERENCED_PARAMETER(ResourcesTranslated);

	PAGED_CODE();

	DbgPrint("[%s] Entry\n", __FUNCTION__);

	DbgPrint("[%s] Exit\n", __FUNCTION__);

	return STATUS_SUCCESS;
}