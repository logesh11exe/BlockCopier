#include "DiskUtils.h"
#include <string> 

// Gets the physical sector size of a volume/disk
DWORD DiskUtils::GetVolumeSectorSize(HANDLE hFile, LPCWSTR path, bool isSrc) {
    LOG_DEBUG(L"Inside GetVolumeSectorSize\n");
    DISK_GEOMETRY diskGeometry;
    DWORD bytesReturned;

    if (DeviceIoControl(hFile, IOCTL_DISK_GET_DRIVE_GEOMETRY, nullptr, 0,
        &diskGeometry, sizeof(diskGeometry), &bytesReturned, nullptr)) {
        LOG_INFO(L"GetVolumeSectorSize : DeviceIoControl Succeeded. Sector Size:%d\n", diskGeometry.BytesPerSector);
        return diskGeometry.BytesPerSector;
    }
    else {
        DWORD err = GetLastError();
        // Check if it's a logical drive letter path (e.g., "\\.\E:") for which this IOCTL often fails.
        if ((err == ERROR_INVALID_PARAMETER || err == ERROR_NOT_SUPPORTED) &&
            path && path[0] == L'\\' && path[1] == L'.' && path[2] == L'\\' && iswalpha(path[3]) && path[4] == L':') {
            LOG_WARNING(L"GetVolumeSectorSize: IOCTL_DISK_GET_DRIVE_GEOMETRY is often not supported for logical drive letter handles. Path: %s. Error:%d\n", path, err);
            LOG_DEBUG(L"End of GetVolumeSectorSize\n");
            return 0; 
        }

        LOG_ERROR(L"GetVolumeSectorSize: Failed to get physical sector size for %s for the path %s with the error : %d\n", (isSrc ? L"source" : L"destination"), path, err);
        LOG_DEBUG(L"End of GetVolumeSectorSize\n");
        return 0;
    }
}

LONGLONG DiskUtils::GetDiskOrDriveSize(HANDLE handle, LPCWSTR path, bool isSrc)
{
    LOG_DEBUG(L"Inside GetDiskOrDriveSize\n");
    if (isSrc)
    {
        LOG_DEBUG(L"GetDiskOrDriveSize: Going to query source size.\n");
        LONGLONG srcSize = 0;
        //Get total folder/volume size from source
        GET_LENGTH_INFORMATION srcLenInfo;
        if (!DeviceIoControl(handle, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0,
            &srcLenInfo, sizeof(srcLenInfo), nullptr, nullptr)) {
            DWORD err = GetLastError();
            LOG_DEBUG(L"GetDiskOrDriveSize: DeviceIoControl failed with IOCTL_DISK_GET_LENGTH_INFO structure for source with error code:%d. Falling back to GetFileSizeEx.\n", err);
            LARGE_INTEGER tempFileSize;
            if (GetFileSizeEx(handle, &tempFileSize)) {
                srcSize = tempFileSize.QuadPart;
                LOG_INFO(L"GetDiskOrDriveSize: GetFileSizeEx Succeeded for source. Size: %lld bytes\n", srcSize);
            }
            else {
                LOG_ERROR(L"Failed to get source size using GetFileSizeEx. Error: %d\n", GetLastError());
                return 0;
            }
        }
        else {
            srcSize = srcLenInfo.Length.QuadPart;
            LOG_INFO(L"GetDiskOrDriveSize: DeviceIoControl Succeeded in querying the source size. Size: %lld bytes\n", srcSize);
        }
        LOG_DEBUG(L"End of GetDiskOrDriveSize\n");
        return srcSize;
    }
    else
    {
        LOG_DEBUG(L"GetDiskOrDriveSize: Going to query destination size.\n");
        LONGLONG destCapacity = 0;

        // Determine if the destination path is a drive letter (e.g., \\.\F:)
        bool isDriveLetter = false;
        if (wcslen(path) >= 5 && path[0] == L'\\' && path[1] == L'.' && path[2] == L'\\') {
            if (iswalpha(path[3]) && path[4] == L':') {
                if (wcslen(path) == 5 || (wcslen(path) == 6 && path[5] == L'\\')) {
                    isDriveLetter = true;
                }
            }
        }

        if (isDriveLetter) {
            LOG_DEBUG(L"GetDiskOrDriveSize: Destination provided is a Drive Letter.\n");
            // For drive letters, GetDiskFreeSpaceExW is generally preferred for total capacity.
            ULARGE_INTEGER numBytesAvailable, totalNumOfBytes, totalNumOfFreeBytes;
            wchar_t driveLetterRoot[4]; // e.g., "F:\\\0"
            wcsncpy_s(driveLetterRoot, path + 2, 2); // Copies "F:"
            driveLetterRoot[2] = L'\\';
            driveLetterRoot[3] = L'\0';

            if (GetDiskFreeSpaceExW(driveLetterRoot, &numBytesAvailable, &totalNumOfBytes, &totalNumOfFreeBytes)) {
                destCapacity = totalNumOfBytes.QuadPart;
                LOG_INFO(L"Got destination size using GetDiskFreeSpaceExW: %lld bytes.\n", destCapacity);
            }
            else {
                DWORD err = GetLastError();
                LOG_DEBUG(L"Failed to get destination size using GetDiskFreeSpaceExW for path %s with error : %d. Falling back to IOCTL_DISK_GET_LENGTH_INFO.\n", path, err);
                // Fallback to IOCTL_DISK_GET_LENGTH_INFO
                GET_LENGTH_INFORMATION destLenInfo;
                if (DeviceIoControl(handle, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0,
                    &destLenInfo, sizeof(destLenInfo), nullptr, nullptr)) {
                    destCapacity = destLenInfo.Length.QuadPart;
                    LOG_INFO(L"Got destination size using IOCTL_DISK_GET_LENGTH_INFO (fallback for logical drive): %lld bytes.\n", destCapacity);
                }
                else {
                    err = GetLastError();
                    LOG_DEBUG(L"Failed to get destination size via IOCTL_DISK_GET_LENGTH_INFO (fallback for logical drive). Error: %d. Falling back to GetFileSizeEx.\n", err);
                    // Final fallback to GetFileSizeEx (might not work for raw disk handles)
                    LARGE_INTEGER tempFileSize;
                    if (GetFileSizeEx(handle, &tempFileSize)) {
                        destCapacity = tempFileSize.QuadPart;
                        LOG_INFO(L"Got destination size using GetFileSizeEx (final fallback for logical drive): %lld bytes.\n", destCapacity);
                    }
                    else {
                        LOG_ERROR(L"Failed to get destination size (final fallback). Error: %d\n", GetLastError());
                        return 0;
                    }
                }
            }
        }
        else { // Not a drive letter, assume it's a direct disk/partition path (\\.\PhysicalDriveX)
            DISK_GEOMETRY_EX diskGeometryEx;
            DWORD bytesReturned;

            // Try IOCTL_DISK_GET_LENGTH_INFO first
            GET_LENGTH_INFORMATION destLenInfo;
            if (DeviceIoControl(handle, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0,
                &destLenInfo, sizeof(destLenInfo), nullptr, nullptr)) {
                destCapacity = destLenInfo.Length.QuadPart;
                LOG_INFO(L"Got destination size using IOCTL_DISK_GET_LENGTH_INFO: %lld bytes.\n", destCapacity);
            }
            else {
                DWORD errLenInfo = GetLastError();
                LOG_DEBUG(L"Failed IOCTL_DISK_GET_LENGTH_INFO Error: %d. Trying IOCTL_DISK_GET_DRIVE_GEOMETRY_EX.\n", errLenInfo);

                // Fallback IOCTL_DISK_GET_DRIVE_GEOMETRY_EX
                if (DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0,
                    &diskGeometryEx, sizeof(diskGeometryEx), &bytesReturned, nullptr)) {
                    destCapacity = diskGeometryEx.DiskSize.QuadPart;
                    LOG_INFO(L"Got destination size using IOCTL_DISK_GET_DRIVE_GEOMETRY_EX: %lld bytes.\n", destCapacity);
                }
                else {  // If this also fails, then you're truly blocked.
                    DWORD errGeometry = GetLastError();
                    LOG_ERROR(L"Failed IOCTL_DISK_GET_DRIVE_GEOMETRY_EX Error: %d. Cannot determine destination size for path: %s\n", errGeometry, path);
                    return 0;
                }
            }
        }
        LOG_DEBUG(L"End of GetDiskOrDriveSize\n");
        return destCapacity;
    }
}