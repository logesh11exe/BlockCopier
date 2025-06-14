# BlockCopier

BlockCopier is a high-performance Windows command-line tool for copying the contents of a VSS (Volume Shadow Copy Service) snapshot or raw disk/volume to an empty disk or partition. It is designed for robust, parallel, block-level copying, making it suitable for backup, disk imaging, or forensic purposes.

## Features
- Copies data from a source (VSS snapshot, raw disk, or file) to a target disk or partition at the block level
- Uses asynchronous, parallel I/O for high performance
- Supports configurable block size and number of parallel threads
- Handles direct I/O with proper buffer alignment
- Verifies destination size and sector alignment to prevent data loss
- Written in modern C++ for Windows

## Requirements
- Windows 10 or later
- Administrator privileges (required for raw disk access)
- Visual Studio 2022 (or compatible) for building from source
- Install Microsoft Visual C++ 2015-2022 Redistributable(x86/x64)

## Building
1. Open `FileBackup.sln` in Visual Studio.
2. Select the desired configuration (Debug/Release, x64/x86).
3. Build the solution (Ctrl+Shift+B).
4. The executable will be located in `FileBackup/Release/` or `FileBackup/Debug/` depending on the configuration.

## Usage
Run the tool from an elevated command prompt (Run as Administrator):

```
FileBackup.exe <sourcePath> <targetPartitionPath> [threads] [blockSizeMB]
```

- `<sourcePath>`: Path to the source VSS snapshot, raw disk, or file (e.g., `\\?\GLOBALROOT\Device\HarddiskVolumeShadowCopyX`)
- `<targetPartitionPath>`: Path to the target disk or partition (e.g., `\\.\PhysicalDriveX`)
- `[threads]`: (Optional) Number of parallel I/O threads (default: 8, max: 64)
- `[blockSizeMB]`: (Optional) Block size in megabytes (default: 1)

**Example:**
```
FileBackup.exe "\\?\GLOBALROOT\Device\HarddiskVolumeShadowCopy1" "\\.\PhysicalDrive2" 8 4
```
This copies from the specified VSS snapshot to the target physical drive using 8 threads and 4MB blocks.

## Notes & Warnings
- **Data Loss Warning:** The target disk/partition will be overwritten. Ensure you specify the correct target!
- **Permissions:** You must run as Administrator to access raw disks and VSS snapshots.
- **Destination Size:** The tool checks that the destination is at least as large as the source. Copying will abort if not.
- **Alignment:** Block size must be a multiple of the destination's physical sector size (usually 4096 bytes).
- **No Buffering:** Uses direct I/O for performance; buffers are page-aligned.



---

For questions or contributions, please open an issue or pull request.
