#pragma once

#include <windows.h>
#include <winioctl.h>
#include <initguid.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <iostream>
//#include "Public.h"

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

//
// Find pico driver device by GUID and return device handle
// Returns INVALID_HANDLE_VALUE if not found
//
inline HANDLE FindPicoDriverDevice(CONST GUID ClassGuid)
{
    HDEVINFO hDevInfo = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA ifData = { 0 };
    PSP_DEVICE_INTERFACE_DETAIL_DATA ifDetailData = NULL;
    DWORD requiredSize = 0;
    DWORD memberIndex = 0;
    HANDLE hDevice = INVALID_HANDLE_VALUE;

    // Get device info set for all devices matching the GUID
    hDevInfo = SetupDiGetClassDevs(
        &ClassGuid,
        NULL,
        NULL,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
    );

    if (hDevInfo == INVALID_HANDLE_VALUE) {
        std::cerr << "SetupDiGetClassDevs failed: " << GetLastError() << std::endl;
        return INVALID_HANDLE_VALUE;
    }

    // Initialize interface data
    ifData.cbSize = sizeof(ifData);

    // Enumerate device interfaces
    if (SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &ClassGuid, memberIndex, &ifData)) {

        // Get the size required for interface detail data
        SetupDiGetDeviceInterfaceDetail(hDevInfo, &ifData, NULL, 0, &requiredSize, NULL);

        // Allocate memory for interface detail data
        ifDetailData = (PSP_DEVICE_INTERFACE_DETAIL_DATA)HeapAlloc(GetProcessHeap(), 0, requiredSize);

        if (ifDetailData == NULL) {
            std::cerr << "HeapAlloc failed" << std::endl;
            SetupDiDestroyDeviceInfoList(hDevInfo);
            return INVALID_HANDLE_VALUE;
        }

        // Set the cbSize field
        ifDetailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

        // Get the interface detail data
        if (SetupDiGetDeviceInterfaceDetail(hDevInfo, &ifData, ifDetailData, requiredSize, &requiredSize, NULL)) {

            // Open device handle
            hDevice = CreateFileW(
                ifDetailData->DevicePath,
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                NULL,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                NULL
            );

            if (hDevice == INVALID_HANDLE_VALUE) {
                std::cerr << "CreateFile failed: " << GetLastError() << std::endl;
            } else {
                std::cout << "Driver device found and opened successfully!" << std::endl;
                std::wcout << "Device path: " << ifDetailData->DevicePath << std::endl;
            }
        } else {
            std::cerr << "SetupDiGetDeviceInterfaceDetail failed: " << GetLastError() << std::endl;
        }

        HeapFree(GetProcessHeap(), 0, ifDetailData);
    } else {
        std::cerr << "SetupDiEnumDeviceInterfaces failed: " << GetLastError() << std::endl;
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);
    return hDevice;
}
