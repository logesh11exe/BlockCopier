#include <windows.h>
#include <iostream>
#include <vector>
#include <atomic>
#include <string>
#include <memory> // Required for std::unique_ptr and std::make_unique
#include <winioctl.h> // Required for DISK_GEOMETRY, GET_LENGTH_INFORMATION, and IOCTL_* definitions

// Define constants for default parameters
#define DEFAULT_BLOCK_SIZE_MB 1 // Default block size of 1 MB
#define DEFAULT_MAX_OUTSTANDING_IO 8 // Default number of parallel I/Os

// Forward declaration of the BlockCopier class
// This is necessary because the static callback functions need to refer to it.
class BlockCopier;

// Structure to hold context for each asynchronous I/O operation
// Now includes a pointer back to the BlockCopier instance
struct IOContext {
    OVERLAPPED overlapped = {}; // OVERLAPPED structure for asynchronous I/O
    char* buffer = nullptr;      // Pointer to the memory buffer for this I/O block
    DWORD bufferSize = 0;        // Size of the buffer (typically BLOCK_SIZE)
    std::atomic<bool> completed; // Flag indicating this specific read-write cycle is done
    LONGLONG readOffset = 0;    // The starting offset where this block was read from the source
    BlockCopier* copierInstance = nullptr; // Pointer back to the BlockCopier instance

    // Custom constructor to allocate the buffer and initialize atomic
    IOContext(DWORD bufSize)
        : bufferSize(bufSize), completed(false), readOffset(0), copierInstance(nullptr) {
        // VirtualAlloc is crucial for FILE_FLAG_NO_BUFFERING as it guarantees page-aligned memory.
        buffer = (char*)VirtualAlloc(nullptr, bufferSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!buffer) {
            // In a real application, you might throw std::bad_alloc or handle more gracefully.
            // For now, we rely on the caller (BlockCopier::Initialize) to check for nullptr.
            std::wcerr << L"Error: Failed to allocate buffer for IOContext.\n";
        }
    }

    // Destructor to deallocate the buffer
    ~IOContext() {
        if (buffer) {
            VirtualFree(buffer, 0, MEM_RELEASE);
            buffer = nullptr;
        }
    }

    // Explicitly delete copy constructor and copy assignment operator.
    // This is important because IOContext manages raw memory (buffer) and contains std::atomic.
    // std::atomic itself makes the class non-copyable by default, but explicitly deleting
    // them makes the intent clear and prevents compiler-generated ones that would be unsafe.
    IOContext(const IOContext&) = delete;
    IOContext& operator=(const IOContext&) = delete;

    // Also delete move constructor and move assignment operator unless truly needed and implemented carefully.
    // For this design with std::unique_ptr, we don't need them for IOContext itself.
    IOContext(IOContext&&) = delete;
    IOContext& operator=(IOContext&&) = delete;
};

// --- DECLARATIONS OF STATIC CALLBACK FUNCTIONS ---
// These declarations must come before any functions (like BlockCopier::IssueRead)
// that use them as arguments. Their definitions will come later.
void CALLBACK StaticReadCompletion(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped);
void CALLBACK StaticWriteCompletion(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped);

// --- BlockCopier Class Definition ---
class BlockCopier {
private:
    // Member variables (formerly global)
    std::atomic<int> m_pendingIOs;       // Counter for currently active I/O operations
    std::atomic<LONGLONG> m_fileOffset;   // Atomically tracked global file offset for reads
    std::atomic<bool> m_readComplete;     // Flag indicating all source data has been read
    std::atomic<bool> m_errorOccurred;    // Flag indicating a critical error has occurred
    HANDLE m_hSource;                    // Handle to the source file/volume
    HANDLE m_hDest;                      // Handle to the destination file/volume
    LONGLONG m_totalFileSize;            // Total size of the source data
    LONGLONG m_destinationCapacity;      // Total capacity of the destination (added for robustness)
    DWORD m_destinationSectorSize;        // Physical sector size of the destination device
    int m_numThreads;                    // Number of parallel I/O threads/contexts
    DWORD m_blockSize;                    // Block size in bytes for each I/O operation
    // Use std::unique_ptr to manage IOContext objects and their buffers automatically.
    // This resolves the "deleted function" error and handles memory cleanup correctly.
    std::vector<std::unique_ptr<IOContext>> m_contexts;

    // Declare the static callback functions as friends.
    // This grants them access to the private members of BlockCopier instances,
    // allowing them to call the actual member callback methods.
    friend void CALLBACK StaticReadCompletion(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped);
    friend void CALLBACK StaticWriteCompletion(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped);

    // --- Private Helper Methods ---

    // Gets the physical sector size of a volume/disk
    DWORD GetVolumeSectorSize(HANDLE hFile, LPCWSTR path, bool isSource) {
        DISK_GEOMETRY diskGeometry;
        DWORD bytesReturned;

        if (DeviceIoControl(hFile, IOCTL_DISK_GET_DRIVE_GEOMETRY, nullptr, 0,
            &diskGeometry, sizeof(diskGeometry), &bytesReturned, nullptr)) {
            return diskGeometry.BytesPerSector;
        }
        else {
            DWORD error = GetLastError();
            // Check if it's a logical drive letter causing the error (common for removable media)
            // If it's a path like \\.\F: and the error is ERROR_INVALID_PARAMETER (87) or
            // ERROR_NOT_SUPPORTED (50), it's likely due to trying IOCTL_DISK_GET_DRIVE_GEOMETRY
            // on a logical volume or removable media.
            if ((error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED) &&
                path && path[0] == L'\\' && path[1] == L'.' && path[2] == L'\\' && iswalpha(path[3]) && path[4] == L':') {
                std::wcerr << L"Warning: IOCTL_DISK_GET_DRIVE_GEOMETRY is often not supported for logical drive letter handles (" << path << L"). Error: " << error
                    << L". Assuming 4096 bytes for direct I/O alignment.\n";
                return 4096; // Common physical sector size for modern drives, or fallback
            }

            std::wcerr << L"Warning: Failed to get physical sector size for " << (isSource ? L"source" : L"destination")
                << L" (" << path << L"). Error: " << error
                << L". Assuming 4096 bytes. This might lead to issues if the actual sector size is different.\n";
            return 4096; // Fallback
        }
    }

    // Issues an asynchronous read operation using the given IOContext
    bool IssueRead(IOContext* ctx) {
        // Use member variables instead of global ones
        if (m_readComplete.load() || m_errorOccurred.load()) return false;

        LONGLONG currentOffset = m_fileOffset.fetch_add(m_blockSize);

        if (currentOffset >= m_totalFileSize) {
            m_readComplete = true;
            return false;
        }

        DWORD bytesToRead = m_blockSize;
        if (currentOffset + bytesToRead > m_totalFileSize) {
            bytesToRead = static_cast<DWORD>(m_totalFileSize - currentOffset);
        }

        ctx->overlapped.Offset = static_cast<DWORD>(currentOffset & 0xFFFFFFFF);
        ctx->overlapped.OffsetHigh = static_cast<DWORD>((currentOffset >> 32) & 0xFFFFFFFF);
        ctx->overlapped.hEvent = nullptr;
        ctx->completed = false;
        ctx->readOffset = currentOffset;

        m_pendingIOs++;

        if (!ReadFileEx(m_hSource, ctx->buffer, bytesToRead, &ctx->overlapped, StaticReadCompletion)) {
            DWORD error = GetLastError();
            if (error != ERROR_HANDLE_EOF) {
                std::wcerr << L"ReadFileEx failed at offset " << currentOffset << L" with error: " << error << std::endl;
                m_errorOccurred = true;
            }
            m_pendingIOs--;
            return false;
        }
        return true;
    }

    // --- Private Callback Implementations (called by static trampolines) ---

    // Handles completion of an asynchronous read operation
    void OnReadCompletion(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped) {
        auto ctx = reinterpret_cast<IOContext*>(lpOverlapped);

        if (dwErrorCode != ERROR_SUCCESS) {
            if (dwErrorCode != ERROR_HANDLE_EOF) {
                std::wcerr << L"Read error for offset " << ctx->readOffset << L": " << dwErrorCode << std::endl;
                m_errorOccurred = true;
            }
            m_pendingIOs--;
            return;
        }

        if (dwNumberOfBytesTransfered == 0) {
            m_readComplete = true;
            m_pendingIOs--;
            return;
        }

        // --- CRITICAL: Handle FILE_FLAG_NO_BUFFERING write alignment ---
        DWORD bytesToWrite = dwNumberOfBytesTransfered;
        if (bytesToWrite % m_destinationSectorSize != 0) {
            DWORD padding = m_destinationSectorSize - (bytesToWrite % m_destinationSectorSize);
            if (bytesToWrite + padding > ctx->bufferSize) {
                std::wcerr << L"Error: Buffer too small for padding at offset " << ctx->readOffset
                    << L". Required size: " << bytesToWrite + padding
                    << L", Available buffer size: " << ctx->bufferSize << std::endl;
                m_errorOccurred = true;
                m_pendingIOs--;
                return;
            }
            memset(ctx->buffer + bytesToWrite, 0, padding);
            bytesToWrite += padding;
        }

        ctx->overlapped.Offset = static_cast<DWORD>(ctx->readOffset & 0xFFFFFFFF);
        ctx->overlapped.OffsetHigh = static_cast<DWORD>((ctx->readOffset >> 32) & 0xFFFFFFFF);
        ctx->overlapped.hEvent = nullptr;

        if (!WriteFileEx(m_hDest, ctx->buffer, bytesToWrite, &ctx->overlapped, StaticWriteCompletion)) {
            std::wcerr << L"WriteFileEx failed for offset " << ctx->readOffset << L" with error: " << GetLastError() << std::endl;
            m_errorOccurred = true;
            m_pendingIOs--;
        }
    }

    // Handles completion of an asynchronous write operation
    void OnWriteCompletion(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped) {
        auto ctx = reinterpret_cast<IOContext*>(lpOverlapped);

        if (dwErrorCode != ERROR_SUCCESS) {
            std::wcerr << L"Write error for offset " << ctx->readOffset << L": " << dwErrorCode << std::endl;
            m_errorOccurred = true;
        }

        ctx->completed = true;
        m_pendingIOs--;
    }

public:
    // --- Constructor ---
    BlockCopier() :
        m_pendingIOs(0), m_fileOffset(0), m_readComplete(false), m_errorOccurred(false),
        m_hSource(INVALID_HANDLE_VALUE), m_hDest(INVALID_HANDLE_VALUE),
        m_totalFileSize(0), m_destinationCapacity(0), m_destinationSectorSize(0),
        m_numThreads(DEFAULT_MAX_OUTSTANDING_IO), m_blockSize(DEFAULT_BLOCK_SIZE_MB * 1024 * 1024) {
    }

    // --- Destructor (Handles resource cleanup using RAII) ---
    // The m_contexts vector (containing unique_ptr) will automatically clean up IOContext objects
    // and their buffers when the BlockCopier object is destroyed.
    ~BlockCopier() {
        if (m_hSource != INVALID_HANDLE_VALUE) {
            CloseHandle(m_hSource);
            m_hSource = INVALID_HANDLE_VALUE;
        }
        if (m_hDest != INVALID_HANDLE_VALUE) {
            CloseHandle(m_hDest);
            m_hDest = INVALID_HANDLE_VALUE;
        }
    }

    // --- Public Initialization Method ---
    bool Initialize(LPCWSTR sourcePath, LPCWSTR destPath, int numThreads = DEFAULT_MAX_OUTSTANDING_IO, int blockSizeMB = DEFAULT_BLOCK_SIZE_MB) {
        m_numThreads = numThreads;
        m_blockSize = static_cast<DWORD>(blockSizeMB) * 1024 * 1024;

        std::wcout << L"Source Path: " << sourcePath << std::endl;
        std::wcout << L"Destination Path: " << destPath << std::endl;
        std::wcout << L"Configured Threads: " << m_numThreads << std::endl;
        std::wcout << L"Requested Block Size: " << m_blockSize / (1024 * 1024) << L" MB\n";

        // Validate parsed parameters
        if (m_numThreads <= 0 || m_numThreads > 64) {
            std::wcerr << L"Invalid number of threads. Must be between 1 and 64.\n";
            return false;
        }
        if (m_blockSize == 0) {
            std::wcerr << L"Invalid block size. Must be a positive integer.\n";
            return false;
        }

        // --- Open handles to source and destination ---
        m_hSource = CreateFileW(sourcePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (m_hSource == INVALID_HANDLE_VALUE) {
            std::wcerr << L"Failed to open source handle (" << sourcePath << L"). Error: " << GetLastError() << std::endl;
            return false;
        }

        m_hDest = CreateFileW(destPath, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, // No share mode for exclusive write
            FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (m_hDest == INVALID_HANDLE_VALUE) {
            std::wcerr << L"Failed to open destination handle (" << destPath << L"). Error: " << GetLastError() << std::endl;
            CloseHandle(m_hSource); // Ensure source handle is closed
            return false;
        }

        // --- Get total file/volume size from source ---
        GET_LENGTH_INFORMATION sourceLengthInfo;
        if (!DeviceIoControl(m_hSource, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0,
            &sourceLengthInfo, sizeof(sourceLengthInfo), nullptr, nullptr)) {
            LARGE_INTEGER tempFileSize;
            if (GetFileSizeEx(m_hSource, &tempFileSize)) {
                m_totalFileSize = tempFileSize.QuadPart;
            }
            else {
                std::wcerr << L"Failed to get source size. Error: " << GetLastError() << std::endl;
                CloseHandle(m_hSource);
                CloseHandle(m_hDest);
                return false;
            }
        }
        else {
            m_totalFileSize = sourceLengthInfo.Length.QuadPart;
        }


        // --- Get total size of the destination disk/volume ---
        m_destinationCapacity = 0; // Initialize to 0

        // Determine if the destination is a logical drive letter (e.g., \\.\F:)
        // This check needs to be precise: "\\.\X:" or "\\.\X:\"
        bool isLogicalDrive = false;
        if (wcslen(destPath) >= 5 && destPath[0] == L'\\' && destPath[1] == L'.' && destPath[2] == L'\\') {
            if (iswalpha(destPath[3]) && destPath[4] == L':') {
                if (wcslen(destPath) == 5 || (wcslen(destPath) == 6 && destPath[5] == L'\\')) {
                    isLogicalDrive = true;
                }
            }
        }

        if (isLogicalDrive) {
            // For logical drive letters, GetDiskFreeSpaceExW is the primary and preferred method.
            ULARGE_INTEGER freeBytesAvailable, totalNumberOfBytes, totalNumberOfFreeBytes;
            wchar_t driveLetterRoot[4]; // e.g., "F:\\0"
            wcsncpy_s(driveLetterRoot, destPath + 2, 2); // Copies "F:"
            driveLetterRoot[2] = L'\\'; // Add trailing backslash required by GetDiskFreeSpaceExW
            driveLetterRoot[3] = L'\0';

            if (GetDiskFreeSpaceExW(driveLetterRoot, &freeBytesAvailable, &totalNumberOfBytes, &totalNumberOfFreeBytes)) {
                m_destinationCapacity = totalNumberOfBytes.QuadPart;
                std::wcout << L"Got destination size using GetDiskFreeSpaceExW.\n";
            }
            else {
                DWORD error = GetLastError();
                std::wcerr << L"Failed to get destination size using GetDiskFreeSpaceExW for " << destPath << L". Error: " << error << std::endl;
                // Fallback to IOCTL_DISK_GET_LENGTH_INFO, might work for some logical volumes, but often fails for removable media.
                GET_LENGTH_INFORMATION destLengthInfo;
                if (DeviceIoControl(m_hDest, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0,
                    &destLengthInfo, sizeof(destLengthInfo), nullptr, nullptr)) {
                    m_destinationCapacity = destLengthInfo.Length.QuadPart;
                    std::wcout << L"Got destination size using IOCTL_DISK_GET_LENGTH_INFO (fallback for logical drive).\n";
                }
                else {
                    error = GetLastError();
                    std::wcerr << L"Failed to get destination size via IOCTL_DISK_GET_LENGTH_INFO (fallback for logical drive). Error: " << error << std::endl;
                    // Final fallback to GetFileSizeEx (for files or very unusual volumes opened as files).
                    LARGE_INTEGER tempFileSize;
                    if (GetFileSizeEx(m_hDest, &tempFileSize)) {
                        m_destinationCapacity = tempFileSize.QuadPart;
                        std::wcout << L"Got destination size using GetFileSizeEx (final fallback for logical drive).\n";
                    }
                    else {
                        std::wcerr << L"Failed to get destination size (final fallback). Error: " << GetLastError() << std::endl;
                        CloseHandle(m_hSource);
                        CloseHandle(m_hDest);
                        return false;
                    }
                }
            }
        }
        else {

            DISK_GEOMETRY_EX diskGeometryEx;
            DWORD bytesReturned;

            // Try IOCTL_DISK_GET_LENGTH_INFO first (as it's direct)
            GET_LENGTH_INFORMATION destLengthInfo;
            if (DeviceIoControl(m_hDest, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0,
                &destLengthInfo, sizeof(destLengthInfo), nullptr, nullptr)) {
                m_destinationCapacity = destLengthInfo.Length.QuadPart;
                std::wcout << L"Got destination size using IOCTL_DISK_GET_LENGTH_INFO: "
                    << m_destinationCapacity << L" bytes.\n";
            }
            else {
                DWORD errorLengthInfo = GetLastError();
                std::wcerr << L"Failed IOCTL_DISK_GET_LENGTH_INFO (Error: " << errorLengthInfo << L"). Trying IOCTL_DISK_GET_DRIVE_GEOMETRY_EX.\n";

                // Fallback to IOCTL_DISK_GET_DRIVE_GEOMETRY_EX
                if (DeviceIoControl(m_hDest, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0,
                    &diskGeometryEx, sizeof(diskGeometryEx), &bytesReturned, nullptr)) {
                    m_destinationCapacity = diskGeometryEx.DiskSize.QuadPart;
                    std::wcout << L"Got destination size using IOCTL_DISK_GET_DRIVE_GEOMETRY_EX: "
                        << m_destinationCapacity << L" bytes.\n";
                }
                else {
                    DWORD errorGeometry = GetLastError();
                    std::wcerr << L"Failed IOCTL_DISK_GET_DRIVE_GEOMETRY_EX (Error: " << errorGeometry << L").\n";
                    // If this also fails, then you're truly blocked.
                    CloseHandle(m_hSource);
                    CloseHandle(m_hDest);
                    return false;
                }
            }
        }

        // --- Size Comparison Check ---
        if (m_destinationCapacity < m_totalFileSize) {
            std::wcerr << L"Error: Destination size (" << m_destinationCapacity / (1024 * 1024)
                << L" MB) is smaller than source size (" << m_totalFileSize / (1024 * 1024)
                << L" MB).\n";
            std::wcerr << L"Copy operation aborted to prevent data truncation.\n";
            CloseHandle(m_hSource);
            CloseHandle(m_hDest);
            return false;
        }
        // --- Get the physical sector size for the destination device ---
        m_destinationSectorSize = GetVolumeSectorSize(m_hDest, destPath, false);
        if (m_destinationSectorSize == 0) {
            std::wcerr << L"Critical error: Failed to determine destination sector size.\n";
            CloseHandle(m_hSource);
            CloseHandle(m_hDest);
            return false;
        }

        // --- Validate Block Size against Destination Sector Size ---
        if (m_blockSize % m_destinationSectorSize != 0) {
            std::wcerr << L"Error: Configured block size (" << m_blockSize << L" bytes) is not a multiple of "
                << L"destination's physical sector size (" << m_destinationSectorSize << L" bytes).\n";
            std::wcerr << L"Please choose a block size that is a multiple of " << m_destinationSectorSize << L".\n";
            CloseHandle(m_hSource);
            CloseHandle(m_hDest);
            return false;
        }

        std::wcout << L"Source size: " << m_totalFileSize / (1024 * 1024) << L" MB\n";
        std::wcout << L"Destination size: " << m_destinationCapacity / (1024 * 1024) << L" MB\n"; // Display destination capacity
        std::wcout << L"Destination physical sector size: " << m_destinationSectorSize << L" bytes\n";
        std::wcout << L"Actual Block Size used: " << m_blockSize / (1024 * 1024) << L" MB\n";

        // --- Allocate I/O contexts and their associated buffers ---
        // Resize the vector to hold numThreads unique_ptrs.
        // Each unique_ptr will then manage an IOContext object.
        m_contexts.reserve(m_numThreads); // Pre-allocate memory to avoid reallocations
        for (int i = 0; i < m_numThreads; ++i) {
            // Use std::make_unique to create an IOContext and put it into a unique_ptr.
            // The IOContext constructor will handle its internal buffer allocation.
            std::unique_ptr<IOContext> newCtx = std::make_unique<IOContext>(m_blockSize);
            if (!newCtx->buffer) { // Check if buffer allocation failed inside IOContext constructor
                std::wcerr << L"Error: Failed to allocate buffer for IOContext " << i << ".\n";
                // Don't set m_errorOccurred here, return false directly to signal initialization failure.
                // The destructor will clean up already allocated contexts.
                return false;
            }
            // Set the back-pointer from IOContext to this BlockCopier instance
            newCtx->copierInstance = this;

            // Diagnostic: Check buffer alignment
            if (reinterpret_cast<uintptr_t>(newCtx->buffer) % m_destinationSectorSize != 0) {
                std::wcerr << L"Warning: Allocated buffer address (" << (void*)newCtx->buffer
                    << L") for context " << i << L" is not aligned to destination sector size ("
                    << m_destinationSectorSize << L")!\n";
            }
            m_contexts.push_back(std::move(newCtx)); // Move the unique_ptr into the vector
        }

        return true; // Initialization successful
    }

    // --- Public Method to Start the Copy Process ---
    bool StartCopy() {
        if (m_hSource == INVALID_HANDLE_VALUE || m_hDest == INVALID_HANDLE_VALUE || m_contexts.empty()) {
            std::wcerr << L"Error: BlockCopier not initialized correctly before calling StartCopy.\n";
            return false;
        }

        std::wcout << L"Starting block copy...\n";

        // --- Issue initial read operations to prime the I/O queue ---
        for (int i = 0; i < m_numThreads; ++i) {
            // Access the raw IOContext pointer from the unique_ptr using .get()
            if (!IssueRead(m_contexts[i].get())) {
                // If IssueRead returns false, it means EOF was reached very early
                // or a critical error occurred during initial issuance.
                break;
            }
        }

        // --- Main processing loop: wait for I/O completions and issue new reads ---
        while ((m_pendingIOs > 0 || !m_readComplete.load()) && !m_errorOccurred.load()) {
            SleepEx(INFINITE, TRUE); // Wait for APCs

            // Iterate through contexts to find completed write operations and re-issue reads
            for (auto& unique_ctx_ptr : m_contexts) { // Iterate through unique_ptr's
                IOContext* ctx = unique_ctx_ptr.get(); // Get the raw pointer to the IOContext
                if (ctx->completed.load() && !m_readComplete.load() && !m_errorOccurred.load()) {
                    ctx->completed = false; // Reset the 'completed' flag
                    if (!IssueRead(ctx)) {
                        // IssueRead returning false means no more reads are possible (EOF or error).
                        // The main loop condition handles the overall exit.
                    }
                }
            }
        }

        // --- Final cleanup and flush ---
        // Wait for any remaining I/O operations to complete after the main loop exits
        while (m_pendingIOs > 0) {
            SleepEx(100, TRUE); // Short sleep to allow APCs to process
        }

        // Flush any remaining buffered data to the destination disk
        if (FlushFileBuffers(m_hDest)) {
            std::wcout << L"Destination buffers flushed successfully.\n";
        }
        else {
            std::wcerr << L"Failed to flush destination buffers. Error: " << GetLastError() << std::endl;
            m_errorOccurred = true;
        }

        if (m_errorOccurred.load()) {
            std::wcout << L"Block copy completed with errors.\n";
            return false;
        }
        else {
            std::wcout << L"Block copy completed successfully.\n";
            return true;
        }
    }
};

// --- Static Callback Functions ---
// These functions are static (or global) because ReadFileEx/WriteFileEx expect C-style function pointers.
// They serve as trampolines to call the actual member functions of the BlockCopier instance.
void CALLBACK StaticReadCompletion(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped) {
    auto ctx = reinterpret_cast<IOContext*>(lpOverlapped);
    // Use the stored copierInstance pointer to call the correct member function.
    if (ctx->copierInstance) {
        ctx->copierInstance->OnReadCompletion(dwErrorCode, dwNumberOfBytesTransfered, lpOverlapped);
    }
}

void CALLBACK StaticWriteCompletion(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped) {
    auto ctx = reinterpret_cast<IOContext*>(lpOverlapped);
    // Use the stored copierInstance pointer to call the correct member function.
    if (ctx->copierInstance) {
        ctx->copierInstance->OnWriteCompletion(dwErrorCode, dwNumberOfBytesTransfered, lpOverlapped);
    }
}

// --- Main application entry point ---
int wmain(int argc, wchar_t* argv[]) {
    // Parse command-line arguments
    if (argc < 3) {
        std::wcerr << L"Usage: " << argv[0] << L" <sourcePath> <targetPartitionPath> [threads] [blockSizeMB]\n";
        std::wcerr << L"Example: " << argv[0] << L" \"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopyX\" \"\\\\.\\PhysicalDriveX\" X Y\n";
        return 1;
    }

    LPCWSTR srcPath = argv[1];
    LPCWSTR dstPath = argv[2];
    int numThreads = (argc > 3) ? _wtoi(argv[3]) : DEFAULT_MAX_OUTSTANDING_IO;
    int blockSizeMB = (argc > 4) ? _wtoi(argv[4]) : DEFAULT_BLOCK_SIZE_MB;

    // Create an instance of the BlockCopier class
    BlockCopier copier;

    // Initialize the copier with paths and parameters
    if (!copier.Initialize(srcPath, dstPath, numThreads, blockSizeMB)) {
        std::wcerr << L"Failed to initialize BlockCopier.\n";
        return 1; // Initialization failed
    }

    // Start the copying process
    if (!copier.StartCopy()) {
        return 1; // Copy failed
    }

    return 0; // Copy successful
}