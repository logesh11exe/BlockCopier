#pragma once
#include <windows.h>
#include <iostream>
#include <atomic>
#include <LogUtils.h> 

// Forward declaration of BlockCopier for IOContext
class BlockCopier;

// Structure to hold context for each asynchronous I/O operation
struct IOContext {
    OVERLAPPED overlapped = {}; // OVERLAPPED structure for async I/O
    char* buf = nullptr;        // Buffer for read/write operations
    DWORD bufSize = 0;          // Size of the buffer
    std::atomic<bool> completed; // flag to signal completion of an operation
    LONGLONG readOffset = 0;    // Offset at which the current read operation started
    DWORD bytesTransferred = 0; // Store the actual bytes transferred for this specific I/O operation 
    BlockCopier* curInst = nullptr; // Pointer to the BlockCopier instance for callbacks

    IOContext(DWORD bSize)
        : bufSize(bSize), completed(false), readOffset(0), bytesTransferred(0), curInst(nullptr) { // Initialize bytesTransferred
        // VirtualAlloc is crucial for FILE_FLAG_NO_BUFFERING as it guarantees page aligned memory.
        buf = (char*)VirtualAlloc(nullptr, bufSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!buf) {
            LOG_ERROR(L"IOContext: Failed to allocate buffer for IOContext.\n");
        }
    }

    ~IOContext() {
        if (buf) {
            VirtualFree(buf, 0, MEM_RELEASE);
            buf = nullptr;
        }
    }

    // Explicitly delete copy constructor and copy assignment operator
    IOContext(const IOContext&) = delete;
    IOContext& operator=(const IOContext&) = delete;
    IOContext(IOContext&&) = delete;
    IOContext& operator=(IOContext&&) = delete;
};

class IOUtils {
private:
    std::atomic<int> m_pendingIOs;         // Counter for currently active I/O operations
    std::atomic<LONGLONG> m_fileOffset;     // Tracked global file offset for reads
    std::atomic<bool> m_readComplete;       // Flag indicating all source data has been read
    std::atomic<bool> m_errOccurred;        // Flag indicating a critical error has occurred

public:
    IOUtils() : m_pendingIOs(0), m_fileOffset(0), m_readComplete(false), m_errOccurred(false) {}

    // Getters
    int getPendingIOs();
    bool getReadCompleteInfo();
    bool getErrorOccuredInfo();
    LONGLONG getFileOffset();

    // Setters
    void setReadCompleteInfo(bool ifCompleted);
    void setErrorOccuredInfo(bool ifOccured);
    void setPendingIOs(int value);
    void setFileOffset(LONGLONG offset);

    // Issues an asynchronous read operation using the given IOContext
    bool IssueRead(const HANDLE& handle, IOContext* cntxt, const DWORD& blockSize, const LONGLONG& totalFileSize);

    // Issues an asynchronous write operation using the given IOContext
    bool IssueWrite(const HANDLE& handle, IOContext* cntxt, DWORD bytesToWrite); 

    // Handlers for completion of asynchronous I/O operations (called by static callbacks)
    void OnReadCompletion(DWORD errCode, DWORD numOfBytesTransfered, LPOVERLAPPED lpOverlapped);
    void OnWriteCompletion(DWORD errCode, DWORD numOfBytesTransfered, LPOVERLAPPED lpOverlapped);

    ~IOUtils() {} // Destructor
};

// These functions are static because ReadFileEx/WriteFileEx expect C-style function pointers.
// They serve as bridge to call the actual member functions of the BlockCopier/IOUtils instance.
void CALLBACK StaticReadCompletion(DWORD errCode, DWORD numOfBytesTransfered, LPOVERLAPPED lpOverlapped);
void CALLBACK StaticWriteCompletion(DWORD errCode, DWORD numOfBytesTransfered, LPOVERLAPPED lpOverlapped);
// A dummy APC completion routine to wake up threads for termination check
void CALLBACK DummyApcCompletion(ULONG_PTR dwParam);