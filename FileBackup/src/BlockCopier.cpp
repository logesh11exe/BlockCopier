#include "BlockCopier.h"
#include <thread> 
#include <chrono> 

DWORD BlockCopier::getDestSectorSize()
{
    return m_destSectorSize;
}

HANDLE BlockCopier::getDestHandle()
{
    return m_hDest;
}

// Each thread runs this loop
void BlockCopier::WorkerThreadLoop(IOContext* context, const HANDLE& hSrc, const HANDLE& hDest, const DWORD& blockSize, const LONGLONG& totalFileSize) {
    LOG_DEBUG(L"Inside BlockCopier::WorkerThreadLoop\n");
    
    LOG_INFO(L"BlockCopier::WorkerThreadLoop: Worker Thread started. ThreadId: %d\n", GetCurrentThreadId());

    context->curInst = this;

    // Issue initial read for this context
    if (!ioUtilsObj.IssueRead(hSrc, context, blockSize, totalFileSize)) {
        LOG_DEBUG(L"BlockCopier::WorkerThreadLoop: Worker Thread %d: Initial IssueRead failed or no more reads. Exiting.\n", GetCurrentThreadId());
        return;
    }


    // Loop indefinitely until global read completion, error, or no more work
    while (!ioUtilsObj.getErrorOccuredInfo() &&
        !(ioUtilsObj.getReadCompleteInfo() && ioUtilsObj.getPendingIOs() == 0))
    {
        // This thread will wake up when an APC for its context completes
        SleepEx(INFINITE, TRUE);

        // An APC has been processed if the context's operation has completed try to issue the next logical operation
        if (context->completed.load(std::memory_order_acquire)) {
            context->completed.store(false, std::memory_order_release); // Reset completion flag

            if (ioUtilsObj.getErrorOccuredInfo()) {
                LOG_ERROR(L"BlockCopier::WorkerThreadLoop: Worker Thread %d: Global error detected, terminating loop.\n", GetCurrentThreadId());
                break; 
            }

            // If a read has just completed or if a write has just completed this context is now free to issue a new READ
            if (!ioUtilsObj.getReadCompleteInfo()) {
                if (!ioUtilsObj.IssueRead(hSrc, context, blockSize, totalFileSize)) {

                    if (ioUtilsObj.getReadCompleteInfo() || ioUtilsObj.getErrorOccuredInfo()) {
                        LOG_DEBUG(L"BlockCopier::WorkerThreadLoop: Worker Thread %d: No more reads to issue or error during read issuance. Exiting loop.\n", GetCurrentThreadId());
                        break; 
                    }
                    else {
                        // This case implies an unexpected failure to issue read that didn't set global error/complete flags
                        LOG_ERROR(L"BlockCopier::WorkerThreadLoop : Worker Thread %d: IssueRead failed unexpectedly. Terminating thread.\n", GetCurrentThreadId());
                        break;
                    }
                }
            }
            else {
                LOG_DEBUG(L"BlockCopier::WorkerThreadLoop: Worker Thread %d: All reads issued. Waiting for remaining pending I/Os for this context.\n", GetCurrentThreadId());
            }
        }
    }

    LOG_INFO(L"BlockCopier::WorkerThreadLoop : Worker Thread %d finished.\n", GetCurrentThreadId());
    LOG_DEBUG(L"End of BlockCopier::WorkerThreadLoop\n");
}


bool BlockCopier::Initialize(LPCWSTR srcPath, LPCWSTR destPath, int nThreads, int blockSizeMB) {
    LOG_DEBUG(L"Inside BlockCopier::Initialize\n");
    m_numOfThreads = nThreads;
    m_blockSize = static_cast<DWORD>(blockSizeMB) * 1024 * 1024;

    LOG_INFO(L"BlockCopier::Initialize: Source Path: %s\n", srcPath);
    LOG_INFO(L"Destination Path: %s\n", destPath);
    LOG_INFO(L"Configured Threads: %d\n", m_numOfThreads);
    LOG_INFO(L"Requested Block Size: %d MB\n", m_blockSize / (1024 * 1024));

    // Validate parameters
    if (m_numOfThreads <= 0 || m_numOfThreads > 64) {
        LOG_ERROR(L"Invalid number of threads. Must be between 1 and 64.\n");
        LOG_DEBUG(L"End of BlockCopier::Initialize:\n");
        return false;
    }
    if (m_blockSize <= 0) {
        LOG_ERROR(L"Invalid block size. Must be a positive integer.\n");
        LOG_DEBUG(L"End of BlockCopier::Initialize:\n");
        return false;
    }

    // Open Source File with FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN
    m_hSrc = CreateFileW(srcPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (m_hSrc == INVALID_HANDLE_VALUE) {
        LOG_ERROR(L"Failed to open source handle for the path:%s with the error:%d\n", srcPath, GetLastError());
        LOG_DEBUG(L"End of BlockCopier::Initialize:\n");
        return false;
    }

    // Open Destination File with FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN
    m_hDest = CreateFileW(destPath, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, // No share mode for exclusive write
        FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (m_hDest == INVALID_HANDLE_VALUE) {
        LOG_ERROR(L"Failed to open destination handle for the path %s with the error :%d\n", destPath, GetLastError());
        CloseHandle(m_hSrc); // Ensure source handle is closed
        LOG_DEBUG(L"End of BlockCopier::Initialize:\n");
        return false;
    }

    // Get total folder/volume size from source
    m_srcFileSize = diskUtilsObj.GetDiskOrDriveSize(m_hSrc, srcPath, TRUE);
    if (m_srcFileSize == 0) {
        LOG_ERROR(L"BlockCopier::Initialize: Failed to determine source file size.\n");
        CloseHandle(m_hSrc);
        CloseHandle(m_hDest);
        return false;
    }

    // Get total folder/volume size from destination
    m_destCapacity = diskUtilsObj.GetDiskOrDriveSize(m_hDest, destPath, FALSE);
    if (m_destCapacity == 0) {
        LOG_ERROR(L"BlockCopier::Initialize: Failed to determine destination capacity.\n");
        CloseHandle(m_hSrc);
        CloseHandle(m_hDest);
        return false;
    }

    // Destination Size Check
    if (m_destCapacity < m_srcFileSize) {
        LOG_ERROR(L"BlockCopier::Initialize: Destination size (%lld MB) is smaller than source size (%lld MB). \n", m_destCapacity / (1024 * 1024), m_srcFileSize / (1024 * 1024));
        LOG_ERROR(L"BlockCopier::Initialize: Copy operation aborted to prevent data truncation.\n");
        CloseHandle(m_hSrc);
        CloseHandle(m_hDest);
        LOG_DEBUG(L"End of BlockCopier::Initialize:\n");
        return false;
    }

    // Physical sector size for the destination disk
    m_destSectorSize = diskUtilsObj.GetVolumeSectorSize(m_hDest, destPath, false);
    if (m_destSectorSize == 0) {
        LOG_ERROR(L"BlockCopier::Initialize: Failed to determine destination sector size. Error: %d\n", GetLastError());
        std::wcerr << L"Since destination sector size query failed, assuming Sector Size as 4096 bytes. This might lead to issues if the actual sector size is different.\n";
        std::wcerr << L"To Proceed press 1, to exit press 0\n";
        int proceed_choice;
        std::wcin >> proceed_choice;
        if (proceed_choice == 0) {
            CloseHandle(m_hSrc);
            CloseHandle(m_hDest);
            LOG_DEBUG(L"End of BlockCopier::Initialize:\n");
            return false;
        }
        else {
            LOG_INFO(L"With user confirmation, considering Sector size as 4096 bytes and proceeding.\n");
            m_destSectorSize = 4096;
        }
    }

    // Validate Block Size against Destination Sector Size
    if (m_blockSize % m_destSectorSize != 0) {
        LOG_ERROR(L"BlockCopier::Initialize: Configured block size (%d bytes) is not a multiple of destination's physical sector size (%d bytes).\n", m_blockSize, m_destSectorSize);
        std::wcerr << L"Please choose a block size that is a multiple of " << m_destSectorSize << L".\n";
        CloseHandle(m_hSrc);
        CloseHandle(m_hDest);
        LOG_DEBUG(L"End of BlockCopier::Initialize:\n");
        return false;
    }

    LOG_INFO(L"Source size: %lld MB\n", m_srcFileSize / (1024 * 1024));
    LOG_INFO(L"Destination size: %lld MB\n", m_destCapacity / (1024 * 1024)); // Display destination capacity
    LOG_INFO(L"Destination physical sector size: %d bytes\n", m_destSectorSize);
    LOG_INFO(L"Actual Block Size used: %d MB\n", m_blockSize / (1024 * 1024));

    // Prepare IOContexts (one for each thread)
    m_cntxts.clear(); // Clear any previous contexts
    m_cntxts.reserve(m_numOfThreads); //allocate memory for performance
    for (int i = 0; i < m_numOfThreads; ++i) {
        std::unique_ptr<IOContext> newCntxt = std::make_unique<IOContext>(m_blockSize);
        if (!newCntxt->buf) { // Check if buffer allocation failed
            LOG_ERROR(L"BlockCopier::Initialize: Failed to allocate buffer for IOContext's Buffer %d\n", i);
            LOG_DEBUG(L"End of BlockCopier::Initialize:\n");
            return false;
        }
        // Set this pointer in IOContext call callbacks
        newCntxt->curInst = this;

        // Check buffer alignment
        if (reinterpret_cast<uintptr_t>(newCntxt->buf) % m_destSectorSize != 0) {
            LOG_ERROR(L"BlockCopier::Initialize: Allocated buffer address (%p) for context %d is not aligned to destination sector size (%d)!\n", newCntxt->buf, i, m_destSectorSize);
            CloseHandle(m_hSrc);
            CloseHandle(m_hDest);
            return false;
        }
        m_cntxts.push_back(std::move(newCntxt)); // Move the ownership from unique_ptr into the vector
    }
    LOG_DEBUG(L"End of BlockCopier::Initialize:\n");
    return true;
}

bool BlockCopier::StartCopy() {
    LOG_DEBUG(L"Inside BlockCopier::StartCopy\n");
    if (m_hSrc == INVALID_HANDLE_VALUE || m_hDest == INVALID_HANDLE_VALUE || m_cntxts.empty()) {
        LOG_ERROR(L"BlockCopier::StartCopy: BlockCopier not initialized correctly before calling StartCopy.\n");
        LOG_DEBUG(L"End of BlockCopier::StartCopy\n");
        return false;
    }

    LOG_INFO(L"BlockCopier::StartCopy: Starting block copy...\n");

    // Reset global IOUtils states for a new copy operation
    ioUtilsObj.setReadCompleteInfo(false);
    ioUtilsObj.setErrorOccuredInfo(false);
    ioUtilsObj.setFileOffset(0); 
    ioUtilsObj.setPendingIOs(0);
    m_bytesReadTotal = 0;  
    m_bytesWrittenTotal = 0;    

    // Launch worker threads
    m_workerThreads.clear(); // Clear any existing threads from previous runs
    m_workerThreads.reserve(m_numOfThreads);
    for (int i = 0; i < m_numOfThreads; ++i) {
        m_workerThreads.emplace_back(&BlockCopier::WorkerThreadLoop, this,
            m_cntxts[i].get(), // Pass the raw pointer to the IOContext for this thread
            std::cref(m_hSrc), std::cref(m_hDest), std::cref(m_blockSize), std::cref(m_srcFileSize));
    }

    // Monitor progress and wait for all operations to complete
    LOG_INFO(L"Main thread monitoring copy progress. Process ID: %d, Thread ID: %d\n", GetCurrentProcessId(), GetCurrentThreadId());
    LONGLONG lastReadPrinted = 0;
    LONGLONG lastWrittenPrinted = 0;

    // Runs as long as there are pending I/Os OR not all reads have been issued,AND no error has occurred. This ensures we wait for all alive I/Os.
    while ((ioUtilsObj.getPendingIOs() > 0 || !ioUtilsObj.getReadCompleteInfo()) &&
        !ioUtilsObj.getErrorOccuredInfo()) {

        // Periodically log progress from the main thread
        LONGLONG currentRead = m_bytesReadTotal.load(std::memory_order_acquire);
        LONGLONG currentWritten = m_bytesWrittenTotal.load(std::memory_order_acquire);

        if (currentRead > lastReadPrinted + m_blockSize * 4 || currentWritten > lastWrittenPrinted + m_blockSize * 4 || // Log every few blocks
            currentRead >= m_srcFileSize || currentWritten >= m_srcFileSize) { // Always log on completion
            LOG_INFO(L"Progress: Read %lld MB of %lld MB (%.2f%%) | Written %lld MB of %lld MB (%.2f%%). Pending IOs: %d\n",
                currentRead / (1024 * 1024), m_srcFileSize / (1024 * 1024),
                (m_srcFileSize > 0 ? (double)currentRead * 100.0 / m_srcFileSize : 0.0),
                currentWritten / (1024 * 1024), m_srcFileSize / (1024 * 1024), // Assuming target size matches source
                (m_srcFileSize > 0 ? (double)currentWritten * 100.0 / m_srcFileSize : 0.0),
                ioUtilsObj.getPendingIOs());
            lastReadPrinted = currentRead;
            lastWrittenPrinted = currentWritten;
        }

        // Short sleep to prevent busy waiting and yield CPU to worker threads
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    LOG_INFO(L"Main thread: Copy loop finished. Final Pending IOs: %d Read Complete: %d with error: %d\n",
        ioUtilsObj.getPendingIOs(), ioUtilsObj.getReadCompleteInfo(), ioUtilsObj.getErrorOccuredInfo());

    // Signal worker threads to terminate if stucked with SleepEx
    if (!ioUtilsObj.getErrorOccuredInfo()) {
        for (int i = 0; i < m_numOfThreads; ++i) {
            // Check if thread is joinable, it might have already exited
            if (m_workerThreads[i].joinable()) {
                // Get thread handle
                HANDLE threadHandle = m_workerThreads[i].native_handle();
                if (threadHandle != INVALID_HANDLE_VALUE) {
                    if (!QueueUserAPC(DummyApcCompletion, threadHandle, NULL)) {
                        DWORD err = GetLastError();
                        if (err == ERROR_GEN_FAILURE) { 
                            LOG_DEBUG(L"Failed to queue termination APC for worker thread %d. Likely thread already exited. Error: %d\n", m_workerThreads[i].get_id(), err);
                        }
                        else {
                            LOG_ERROR(L"Failed to queue termination APC for worker thread %d. Unexpected error: %d\n", m_workerThreads[i].get_id(), err);
                        }
                    }
                    else {
                        LOG_DEBUG(L"Queued termination APC for worker thread %d.\n", m_workerThreads[i].get_id());
                    }
                }
            }
        }
    }
    // Give a very small moment for APCs to be delivered before joining
    std::this_thread::sleep_for(std::chrono::milliseconds(50));


    // Join all worker threads to ensure they all have finished their work
    for (auto& t : m_workerThreads) {
        if (t.joinable()) {
            t.join();
            LOG_DEBUG(L"Joined worker thread: %d\n", t.get_id()); 
        }
    }

    // Final FlushFileBuffers after all worker threads are done
    if (FlushFileBuffers(m_hDest)) {
        LOG_INFO(L"BlockCopier::StartCopy: Destination buffers flushed successfully.\n");
    }
    else {
        LOG_ERROR(L"BlockCopier::StartCopy: Failed to flush destination buffers. Error: %d\n", GetLastError());
        ioUtilsObj.setErrorOccuredInfo(true);
    }

    if (ioUtilsObj.getErrorOccuredInfo()) {
        LOG_ERROR(L"BlockCopier::StartCopy: Block copy completed with errors.\n");
        LOG_DEBUG(L"End of BlockCopier::StartCopy\n");
        return false;
    }
    else {
        LOG_INFO(L"BlockCopier::StartCopy: Block copy completed successfully.\n"); 
        LOG_DEBUG(L"End of BlockCopier::StartCopy\n");
        return true;
    }
    LOG_DEBUG(L"End of BlockCopier::StartCopy\n");
}