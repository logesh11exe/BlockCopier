#pragma once
#include <windows.h>
#include <iostream>
#include <winioctl.h> // Required for DeviceIoControl related structures
#include <LogUtils.h>

class DiskUtils
{
public:
    DiskUtils() {}

    DWORD GetVolumeSectorSize(HANDLE hFile, LPCWSTR path, bool isSrc);
    LONGLONG GetDiskOrDriveSize(HANDLE handle, LPCWSTR path, bool isSrc);

    ~DiskUtils() {}
};