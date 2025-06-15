#include "IOUtils.h"
#include "BlockCopier.h" // Needed to cast curInst back to BlockCopier*

//getters
int IOUtils::getPendingIOs()
{
    return m_pendingIOs.load(std::memory_order_acquire);
}

bool IOUtils::getReadCompleteInfo()
{
    return m_readComplete.load(std::memory_order_acquire);
}

bool IOUtils::getErrorOccuredInfo()
{
    return m_errOccurred.load(std::memory_order_acquire); 
}

LONGLONG IOUtils::getFileOffset()
{
    return m_fileOffset.load(std::memory_order_acquire);
}

//Setters
void IOUtils::setReadCompleteInfo(bool ifCompleted)
{
    m_readComplete.store(ifCompleted, std::memory_order_release); 
}

void IOUtils::setErrorOccuredInfo(bool ifOccured)
{
    m_errOccurred.store(ifOccured, std::memory_order_release);
}

void IOUtils::setPendingIOs(int value)
{
    m_pendingIOs.store(value, std::memory_order_acquire);
}
void IOUtils::setFileOffset(LONGLONG offset)
{
    m_fileOffset.store(offset, std::memory_order_acquire);
}

// Issues an asynchronous read operation using the given IOContext
bool IOUtils::IssueRead(const HANDLE& handle, IOContext* cntxt, const DWORD& blockSize, const LONGLONG& totalFileSize) {
    LOG_DEBUG(L"Inside IOUtils::IssueRead, Thread ID: %d\n", GetCurrentThreadId());

    if (m_readComplete.load(std::memory_order_acquire) || m_errOccurred.load(std::memory_order_acquire))
    {
        LOG_DEBUG(L"IOUtils::IssueRead: Read already completed or error occurred. Returning false.\n");
        return false;
    }

    // increment the global file offset to claim a block
    LONGLONG curOffset = m_fileOffset.fetch_add(blockSize, std::memory_order_relaxed);

    if (curOffset >= totalFileSize) {
        m_readComplete.store(true, std::memory_order_release); // Mark read as complete
        LOG_DEBUG(L"IOUtils::IssueRead: Current offset (%lld) exceeded total file size (%lld). No more reads to issue.\n", curOffset, totalFileSize);
        return false;
    }

    DWORD bytesToRead = blockSize;
    if (curOffset + bytesToRead > totalFileSize) {
        bytesToRead = static_cast<DWORD>(totalFileSize - curOffset);
    }
    if (bytesToRead == 0) {
        m_readComplete.store(true, std::memory_order_release);
        LOG_DEBUG(L"IOUtils::IssueRead: Calculated bytesToRead is 0. Marking read complete.\n");
        return false;
    }

    cntxt->overlapped.Offset = static_cast<DWORD>(curOffset & 0xFFFFFFFF);
    cntxt->overlapped.OffsetHigh = static_cast<DWORD>((curOffset >> 32) & 0xFFFFFFFF);
    cntxt->overlapped.hEvent = nullptr; // For APCs, hEvent should be null if it's not null it'll just notify and wont trigger callbacks
    cntxt->completed.store(false, std::memory_order_release); 
    cntxt->readOffset = curOffset; 
    cntxt->bytesTransferred = 0; 

    m_pendingIOs.fetch_add(1, std::memory_order_relaxed); // Increment pending IOs before issuing

    if (!ReadFileEx(handle, cntxt->buf, bytesToRead, &cntxt->overlapped, StaticReadCompletion)) {
        DWORD err = GetLastError();
        if (err != ERROR_HANDLE_EOF) {
            LOG_ERROR(L"ReadFileEx failed at offset %lld with error:%d. Thread ID: %d\n", curOffset, err, GetCurrentThreadId());
            m_errOccurred.store(true, std::memory_order_release);
        }
        else {
            LOG_DEBUG(L"ReadFileEx hit EOF at offset %lld. Thread ID: %d\n", curOffset, GetCurrentThreadId());
            m_readComplete.store(true, std::memory_order_release);
        }
        m_pendingIOs.fetch_sub(1, std::memory_order_relaxed); // Decrement on immediate failure
        return false;
    }
    LOG_DEBUG(L"IOUtils::IssueRead: Successfully issued read for offset %lld, Bytes: %d. Pending IOs: %d. Thread ID: %d\n", curOffset, bytesToRead, m_pendingIOs.load(), GetCurrentThreadId());
    return true;
}

// Issues an asynchronous write operation using the given IOContext
bool IOUtils::IssueWrite(const HANDLE& handle, IOContext* cntxt, DWORD bytesToWrite) {
    LOG_DEBUG(L"Inside IOUtils::IssueWrite, Thread ID: %d\n", GetCurrentThreadId());

    if (m_errOccurred.load(std::memory_order_acquire)) {
        LOG_DEBUG(L"IOUtils::IssueWrite: Error already occurred. Returning false.\n");
        return false;
    }

    cntxt->overlapped.hEvent = nullptr; // Ensure hEvent is null for APCs
    cntxt->completed.store(false, std::memory_order_release); 

    m_pendingIOs.fetch_add(1, std::memory_order_relaxed); // Increment pending IOs before issuing

    LOG_DEBUG(L"IOUtils::IssueWrite: Issuing Write for offset %lld, Bytes: %d. Thread ID: %d\n",
        (static_cast<LONGLONG>(cntxt->overlapped.OffsetHigh) << 32) | cntxt->overlapped.Offset,
        bytesToWrite, GetCurrentThreadId());

    if (!WriteFileEx(handle, cntxt->buf, bytesToWrite, &cntxt->overlapped, StaticWriteCompletion)) {
        DWORD err = GetLastError();
        LOG_ERROR(L"WriteFileEx failed for offset %lld with error : %d. Thread ID: %d\n",
            (static_cast<LONGLONG>(cntxt->overlapped.OffsetHigh) << 32) | cntxt->overlapped.Offset,
            err, GetCurrentThreadId());
        m_errOccurred.store(true, std::memory_order_release);
        m_pendingIOs.fetch_sub(1, std::memory_order_relaxed); // Decrement on immediate failure
        return false;
    }
    LOG_DEBUG(L"IOUtils::IssueWrite: Successfully issued write. Pending IOs: %d. Thread ID: %d\n", m_pendingIOs.load(), GetCurrentThreadId());
    return true;
}


//On Completion Callbacks (called by static bridge functions)

void IOUtils::OnReadCompletion(DWORD errCode, DWORD numOfBytesTransfered, LPOVERLAPPED lpOverlapped) {
    LOG_DEBUG(L"Inside IOUtils::OnReadCompletion, Thread ID: %d\n", GetCurrentThreadId());
    auto cntxt = reinterpret_cast<IOContext*>(lpOverlapped);
    if (!cntxt || !cntxt->curInst) {
        LOG_ERROR(L"IOUtils::OnReadCompletion: IOContext object or BlockCopier Instance is invalid. Returning.\n");
        return; 
    }

    // Decrement pending IO count for this operation
    m_pendingIOs.fetch_sub(1, std::memory_order_relaxed);

    if (errCode != ERROR_SUCCESS) {
        if (errCode != ERROR_HANDLE_EOF) {
            LOG_ERROR(L"IOUtils::OnReadCompletion: Read error for offset %lld : %d. Thread ID: %d\n", cntxt->readOffset, errCode, GetCurrentThreadId());
            m_errOccurred.store(true, std::memory_order_release); 
        }
        else {
            LOG_DEBUG(L"IOUtils::OnReadCompletion: Reached EOF. Offset: %lld, Bytes: %d. Thread ID: %d\n", cntxt->readOffset, numOfBytesTransfered, GetCurrentThreadId());
            if (numOfBytesTransfered == 0) { // If 0/nobytes transferred, means all reads done
                m_readComplete.store(true, std::memory_order_release);
            }
        }
        cntxt->completed.store(true, std::memory_order_release); // Signal completion (even on error/EOF)
        return;
    }

    if (numOfBytesTransfered == 0) {
        m_readComplete.store(true, std::memory_order_release);
        cntxt->completed.store(true, std::memory_order_release); 
        LOG_DEBUG(L"IOUtils::OnReadCompletion: 0 bytes transferred. Marking read complete. Thread ID: %d\n", GetCurrentThreadId());
        return;
    }

    // Update global total bytes read for this block copier instance
    cntxt->curInst->m_bytesReadTotal.fetch_add(numOfBytesTransfered, std::memory_order_relaxed);

    // Handling FILE_FLAG_NO_BUFFERING write alignment
    DWORD bytesToWritePadded = numOfBytesTransfered;
    DWORD destSectorSize = cntxt->curInst->getDestSectorSize();
    if (destSectorSize == 0) {
        destSectorSize = 4096; // Fallback
        LOG_WARNING(L"IOUtils::OnReadCompletion: Destination sector size is 0, defaulting to 4096 bytes for padding. Thread ID: %d\n", GetCurrentThreadId());
    }

    if (bytesToWritePadded % destSectorSize != 0) {
        DWORD padding = destSectorSize - (bytesToWritePadded % destSectorSize);
        if (bytesToWritePadded + padding > cntxt->bufSize) {
            LOG_ERROR(L"IOUtils::OnReadCompletion: Buffer too small for padding at offset %lld. Required size: %d, Available buffer size:%d. Thread ID: %d\n", cntxt->readOffset, bytesToWritePadded + padding, cntxt->bufSize, GetCurrentThreadId());
            m_errOccurred.store(true, std::memory_order_release);
            cntxt->completed.store(true, std::memory_order_release);
            return;
        }
        memset(cntxt->buf + bytesToWritePadded, 0, padding);
        bytesToWritePadded += padding;
    }
    cntxt->bytesTransferred = bytesToWritePadded; // Store the actual padded bytes to write

    // Now procced with the corresponding write operation
    if (!cntxt->curInst->ioUtilsObj.IssueWrite(cntxt->curInst->getDestHandle(), cntxt, cntxt->bytesTransferred)) {
        LOG_ERROR(L"IOUtils::OnReadCompletion: Failed to issue WriteFileEx for offset %lld. Thread ID: %d\n", cntxt->readOffset, GetCurrentThreadId());
        m_errOccurred.store(true, std::memory_order_release);
    }
    LOG_DEBUG(L"End of IOUtils::OnReadCompletion: Issued write for offset %lld. Thread ID: %d\n", cntxt->readOffset, GetCurrentThreadId());
}

void IOUtils::OnWriteCompletion(DWORD errCode, DWORD numOfBytesTransfered, LPOVERLAPPED lpOverlapped) {
    LOG_DEBUG(L"Inside IOUtils::OnWriteCompletion, Thread ID: %d\n", GetCurrentThreadId());
    auto cntxt = reinterpret_cast<IOContext*>(lpOverlapped);
    if (!cntxt || !cntxt->curInst) {
        LOG_ERROR(L"IOUtils::OnWriteCompletion: IOContext object or BlockCopier Instance is invalid. Returning.\n");
        return;
    }

    m_pendingIOs.fetch_sub(1, std::memory_order_relaxed); // Decrement pending IOs

    if (errCode != ERROR_SUCCESS) {
        LOG_ERROR(L"IOUtils::OnWriteCompletion: Write error for offset %lld : %d. Thread ID: %d\n", cntxt->readOffset, errCode, GetCurrentThreadId());
        m_errOccurred.store(true, std::memory_order_release); 
    }

    //numOfBytesTransfered here is what Windows actually wrote which should be the updated
    cntxt->curInst->m_bytesWrittenTotal.fetch_add(numOfBytesTransfered, std::memory_order_relaxed);

    cntxt->completed.store(true, std::memory_order_release); 
    LOG_DEBUG(L"End of IOUtils::OnWriteCompletion: Write completed for offset %lld. Pending IOs: %d. Thread ID: %d\n", cntxt->readOffset, m_pendingIOs.load(), GetCurrentThreadId());
}

//Static Callbacks (Bridge functions)
void CALLBACK StaticReadCompletion(DWORD errCode, DWORD numOfBytesTransfered, LPOVERLAPPED lpOverlapped) {
    LOG_DEBUG(L"Inside StaticReadCompletion, Thread ID: %d\n", GetCurrentThreadId());
    auto cntxt = reinterpret_cast<IOContext*>(lpOverlapped);
    if (!cntxt || !cntxt->curInst) {
        LOG_ERROR(L"StaticReadCompletion: Invalid IOContext or BlockCopier instance. Thread ID: %d\n", GetCurrentThreadId());
        return;
    }

    // Call the member function on the BlockCopier's IOUtils object
    cntxt->curInst->ioUtilsObj.OnReadCompletion(errCode, numOfBytesTransfered, lpOverlapped);
    LOG_DEBUG(L"End of StaticReadCompletion, Thread ID: %d\n", GetCurrentThreadId());
}

void CALLBACK StaticWriteCompletion(DWORD errCode, DWORD numOfBytesTransfered, LPOVERLAPPED lpOverlapped) {
    LOG_DEBUG(L"Inside StaticWriteCompletion, Thread ID: %d\n", GetCurrentThreadId());
    auto cntxt = reinterpret_cast<IOContext*>(lpOverlapped);
    if (!cntxt || !cntxt->curInst) {
        LOG_ERROR(L"StaticWriteCompletion: Invalid IOContext or BlockCopier instance. Thread ID: %d\n", GetCurrentThreadId());
        return;
    }

    // Call the member function on the BlockCopier's IOUtils object
    cntxt->curInst->ioUtilsObj.OnWriteCompletion(errCode, numOfBytesTransfered, lpOverlapped);
    LOG_DEBUG(L"End of StaticWriteCompletion, Thread ID: %d\n", GetCurrentThreadId());
}

void CALLBACK DummyApcCompletion(ULONG_PTR dwParam) {
    // This function's purpose is to wake up the thread that called SleepEx.
    LOG_DEBUG(L"DummyApcCompletion received by thread %d. Re-evaluating loop condition.\n", GetCurrentThreadId());
}