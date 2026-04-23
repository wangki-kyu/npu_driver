#include "Driver.h"

#ifdef ALLOC_PRAGMA	
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (PAGE, npudriverEvtDeviceAdd)
#pragma alloc_text (PAGE, npudriverEvtDriverContextCleanup)
#endif

NTSTATUS
DriverEntry(
	_In_ PDRIVER_OBJECT DriverObject,
	_In_ PUNICODE_STRING RegistryPath
)
{
	DbgPrint("[%s] Entry\n", __FUNCTION__);

	WDF_DRIVER_CONFIG config;
	NTSTATUS status;
	WDF_OBJECT_ATTRIBUTES attributes;


	// Register a cleanup callback 
	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	attributes.EvtCleanupCallback = npudriverEvtDriverContextCleanup;

	WDF_DRIVER_CONFIG_INIT(&config, npudriverEvtDeviceAdd);

	status = WdfDriverCreate(DriverObject, RegistryPath, &attributes, &config, WDF_NO_HANDLE);

	if (!NT_SUCCESS(status)) {
		DbgPrint("WdfDriverCreate failed 0x%x\n", status);
		return status;
	}

	DbgPrint("[%s] Exit\n", __FUNCTION__);

	return status;
}

NTSTATUS
npudriverEvtDeviceAdd(
	_In_ WDFDRIVER Driver,
	_Inout_ PWDFDEVICE_INIT DeviceInit
)
{
	NTSTATUS status;

	UNREFERENCED_PARAMETER(Driver);
	PAGED_CODE();	

	DbgPrint("[%s] Entry\n", __FUNCTION__);

	status = npudriverCreateDevice(DeviceInit);

	DbgPrint("[%s] Exit\n", __FUNCTION__);

	return status;
}

VOID
npudriverEvtDriverContextCleanup(
	WDFOBJECT DriverObject
)
{
	UNREFERENCED_PARAMETER(DriverObject);

	PAGED_CODE();

	DbgPrint("[%s] Entry\n", __FUNCTION__);
}

