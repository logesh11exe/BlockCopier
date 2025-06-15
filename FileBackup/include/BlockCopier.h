#pragma once
#include <DiskUtils.h>
#include <IOUtils.h>
#include <LogUtils.h>
#include <vector>
#include <string>
#include <memory>
#include <thread>
#include <atomic>

#define DEFAULT_BLOCK_SIZE_MB 1
#define DEFAULT_MAX_OUTSTANDING_IO 4

class BlockCopier {
private:
    HANDLE m_hSrc;                      
    HANDLE m_hDest;                     
    LONGLONG m_srcFileSize;           
    LONGLONG m_destCapacity;            
    DWORD m_destSectorSize;  // Physical sector size
    int m_numOfThreads;                 
    DWORD m_blockSize;              

    std::vector<std::unique_ptr<IOContext>> m_cntxts; // IOContexts, one for each worker thread
    std::vector<std::thread> m_workerThreads;        

public:
    IOUtils ioUtilsObj;     // for handling I/O operations
    DiskUtils diskUtilsObj; // for disk information

    // For progress reporting
    std::atomic<LONGLONG> m_bytesReadTotal;
    std::atomic<LONGLONG> m_bytesWrittenTotal;


    BlockCopier() :
        m_hSrc(INVALID_HANDLE_VALUE), m_hDest(INVALID_HANDLE_VALUE),
        m_srcFileSize(0), m_destCapacity(0), m_destSectorSize(0),
        m_numOfThreads(DEFAULT_MAX_OUTSTANDING_IO), m_blockSize(DEFAULT_BLOCK_SIZE_MB * 1024 * 1024),
        m_bytesReadTotal(0), m_bytesWrittenTotal(0) { 
    }


    //Getters
    HANDLE getDestHandle();
    DWORD getDestSectorSize();

    // Initialization and main copy logic
    bool Initialize(LPCWSTR srcPath, LPCWSTR destPath, int nThreads = DEFAULT_MAX_OUTSTANDING_IO, int blockSizeMB = DEFAULT_BLOCK_SIZE_MB);
    bool StartCopy();

    ~BlockCopier() {
        // Ensure all worker threads are joined before destruction
        for (auto& t : m_workerThreads) {
            if (t.joinable()) {
                t.join();
            }
        }

        // Close handles
        if (m_hSrc != INVALID_HANDLE_VALUE) {
            CloseHandle(m_hSrc);
            m_hSrc = INVALID_HANDLE_VALUE;
        }
        if (m_hDest != INVALID_HANDLE_VALUE) {
            CloseHandle(m_hDest);
            m_hDest = INVALID_HANDLE_VALUE;
        }
    }

    void WorkerThreadLoop(IOContext* context, const HANDLE& hSrc, const HANDLE& hDest, const DWORD& blockSize, const LONGLONG& totalFileSize);
};