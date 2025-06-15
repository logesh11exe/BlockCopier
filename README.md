# 📦 FileBackup

A high-performance file backup utility that supports asynchronous I/O operations and parallel processing for efficient file copying.

## ✨ Features

- 🔄 Asynchronous I/O operations using Windows Asynchronous Procedure Calls (APCs)
- ⚡ Parallel processing with configurable number of worker threads
- 📂 Support for large files and volumes
- 📊 Progress tracking and detailed logging
- 🔧 Sector-aligned I/O for optimal performance
- 💾 Memory-efficient buffer management
- 🛡️ Error handling and recovery mechanisms
- 🔄 Block-level copying for efficient volume backups
- 📦 Support for both file and raw device copying

## 🖥️ System Requirements

### Development Environment
- 🪟 Windows 10 or later (Tested on Windows 10 64-bit)
- 🛠️ Visual Studio 2019 or later (Tested with Visual Studio 2019 Professional)
- 📦 Windows SDK 10.0.19041.0 or later
- ⚙️ C++17 or later

### Runtime Requirements
- 🪟 Windows 10 or later (64-bit)
- 🔑 Administrator privileges (for volume operations)
- 💾 Sufficient disk space for source and destination

## 📚 Build Dependencies

### Required Libraries
- 🪟 Windows API (included in Windows SDK)
- 📚 Standard C++ Library
- 🔧 Microsoft Visual C++ Runtime

## 🏗️ Building the Project

### Using Visual Studio

1. Clone the repository:
```bash
git clone https://github.com/logesh11exe/BlockCopier.git
cd FileBackup
```

2. Open the solution in Visual Studio:
```bash
FileBackup.sln
```

3. Configure the build:
   - Select "Release" configuration
   - Select "x64" platform for x64 build and "x86" for x86 Build
   - Ensure Windows SDK version matches your system

4. Build the solution:
   - Press F7 or select Build > Build Solution
   - Alternatively, use the command line:
	 - For x64
     ```bash
     msbuild FileBackup.sln /p:Configuration=Release /p:Platform=x64
     ```
	 - For x86
     ```bash
     msbuild FileBackup.sln /p:Configuration=Release /p:Platform=x86
     ```

## 📥 Installation

1. Build the project using the method above

2. The executable will be located at:
  - For x64 Build
   ```
   x64/Release/FileBackup.exe
   ```
   
  - For x86 Build
   ```
   Release/FileBackup.exe
   ```

3. Copy the executable to your desired location

## 🧪 Testing

### Manual Testing

1. 📸 SnapShot to Empty Removable Drive Copy Test:
```bash
FileBackup.exe \\?\GLOBALROOT\Device\HarddiskVolumeShadowCopy8 \\.\F: 20 2
```
> **Note:** 
> - You should provide the path of an existing snapshot as the first parameter
> - Should provide the logical drive letter path to an External Removable Drive as second parameter
> - Number of threads as third parameter
> - Block size in MB as fourth parameter

2. 💽 Snapshot to Empty HardDisk:
```bash
FileBackup.exe \\?\GLOBALROOT\Device\HarddiskVolumeShadowCopy8 \\.\PHYSICALDRIVE1 20 2
```
> **Note:** The only change here is to provide the DeviceObject's path as the destination path for a hard disk (since it's a block copy to a raw harddrive)

3. ⚙️ Default Parameters:
```bash
FileBackup.exe \\?\GLOBALROOT\Device\HarddiskVolumeShadowCopy8 \\.\PHYSICALDRIVE1 --usedefault
```
> **Note:** Here `--usedefault` param uses 4 threads and 1MB as default values and proceeds to copy.

### Validation Steps

1. ✅ File Integrity Check:
   - Compare source and destination file sizes
   
   **Verify File Integrity (Size Comparison)**
   This is the quickest initial check. If sizes don't match, the content definitely won't.

   ```bash
   # Get Source File Size
   dir "E:\"
   # Note: Before taking the snapshot you might have known the exact source path. 
   # Kindly provide that exact path here

   # Get Destination File Size
   dir "F:\"
   # Note: After copying it to a disk. Provide that path as the path.
   ```

   **Expected Result:** The byte counts for both files should be identical.
   > ⚠️ If sizes differ: The copy operation failed to create an exact duplicate. The file integrity check has failed.

2. 📊 Performance Validation:
   - Monitor CPU usage (should be balanced across threads)
   - Check memory usage (should be stable)
   - Verify I/O throughput

3. 🚨 Error Handling Test:
   - Test with non-existent source
   - Test with insufficient permissions

## 📁 Project Structure

```
FileBackup/
├── include/
│   ├── BlockCopier.h    # Main file copy logic and thread management
│   ├── DiskUtils.h      # Disk and volume information utilities
│   ├── IOUtils.h        # Asynchronous I/O operations
│   └── LogUtils.h       # Logging system
├── src/
│   ├── BlockCopier.cpp  # Implementation of file copy logic
│   ├── DiskUtils.cpp    # Disk utilities implementation
│   ├── IOUtils.cpp      # I/O operations implementation
│   ├── LogUtils.cpp     # Logging system implementation
│   └── main.cpp         # Application entry point
```

## ⚙️ Configuration Options

- `DEFAULT_BLOCK_SIZE_MB`: Default block size for I/O operations (default: 1MB)
- `DEFAULT_MAX_OUTSTANDING_IO`: Maximum number of concurrent I/O operations (default: 4)

## 🔧 Troubleshooting

### Common Build Issues

1. 🚫 Windows SDK Version Mismatch:
   - Error: "Windows SDK version not found"
   - Solution: Install the correct Windows SDK version or update project settings

2. 🚫 Missing Dependencies:
   - Error: "Cannot open include file"
   - Solution: Ensure all required libraries are installed

### Runtime Issues

1. 🚫 Access Denied:
   - Error: "Access is denied"
   - Solution: Run as administrator

2. 🚫 Insufficient Memory:
   - Error: "Not enough memory"
   - Solution: Format the target disk.

## 🤝 Contributing

1. Fork the repository
2. Create your feature branch
3. Commit your changes
4. Push to the branch
5. Create a Pull Request

## 💬 Support

For issues and feature requests, please use the GitHub issue tracker.

## 🔄 Block Copy Operations

### What is Block Copy?
Block copy is a low-level copying mechanism that operates at the disk sector level, bypassing the file system. This approach offers several advantages:

- 🚀 **Performance**: Direct sector access for faster copying
- 💾 **Efficiency**: No file system overhead
- 🔒 **Reliability**: Sector-by-sector verification possible
- 📦 **Completeness**: Captures all data, including hidden files and system files

### Supported Block Copy Operations

1. 📸 **Volume Snapshot to Removable Drive**
   - Copies entire volume contents
   - Preserves partition structure
   - Supports sector-by-sector verification
   ```bash
   FileBackup.exe \\?\GLOBALROOT\Device\HarddiskVolumeShadowCopy8 \\.\F: 20 2
   ```

2. 💽 **Volume Snapshot to Raw Disk**
   - Direct disk-to-disk copying
   - Preserves all disk structures
   - Ideal for system backups
   ```bash
   FileBackup.exe \\?\GLOBALROOT\Device\HarddiskVolumeShadowCopy8 \\.\PHYSICALDRIVE1 20 2
   ```

### Block Copy Parameters

- **Thread Count**: Number of parallel copy operations (default: 4)
- **Block Size**: Size of each copy operation in MB (default: 1MB)

### Best Practices

1. 🎯 **Block Size Selection**
   - Larger blocks (20MB+) for sequential access
   - Smaller blocks (1-4MB) for random access
   - Consider available memory when choosing block size

2. ⚡ **Thread Count Optimization**
   - Match thread count to available CPU cores
   - Consider I/O subsystem capabilities
   - Balance between CPU and I/O bottlenecks
