#include "BlockCopier.h"

//Application entry point
int wmain(int argc, wchar_t* argv[]) {
    // Parse commandline arguments
    if (argc < 3 || argc > 5) { // Minimum 3 args (src, dst), Max 5 args (src, dst, threads, block_size) or 4 (src, dst, --usedefault)
        std::wcout<<L"Usage: "<<argv[0]  <<L"<sourcePath> <targetPartitionPath>[--usedefault | <threads> <blockSizeMB>]\n";
        std::wcout<<L"Example 1 (defaults): "<< argv[0] << L"\"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopyX\" \"\\\\.\\PhysicalDriveX\" --usedefault\n";
        std::wcout<<L"Example 2 (custom): "<< argv[0]<<L"\"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopyX\" \"\\\\.\\PhysicalDriveX\" 10 4\n";
        return 1;
    }

    LPCWSTR srcPath = argv[1];
    LPCWSTR dstPath = argv[2];
    int numThreads;
    int blockSizeMB;

    // Check for --usedefault flag
    if (argc == 4 && std::wstring(argv[3]) == L"--usedefault") {
        numThreads = DEFAULT_MAX_OUTSTANDING_IO;
        blockSizeMB = DEFAULT_BLOCK_SIZE_MB;
        std::wcout<<L"Using default parameters: Threads = "<<numThreads<<L", Block Size = "<<blockSizeMB<<L" MB.\n\n";
    }
    // Check if custom threads and block size are provided
    else if (argc == 5) {
        numThreads = _wtoi(argv[3]);
        blockSizeMB = _wtoi(argv[4]);

        // Basic validation for custom values
        if (numThreads <= 0 || blockSizeMB <= 0) {
            std::wcout<<L"Invalid threads ("<<numThreads<<L") or block size("<<blockSizeMB<<L" MB).Must be positive integers.\n\n";
            return 1;
        }
        std::wcout<<L"Using custom parameters: Threads = "<<numThreads<<L", Block Size = "<<blockSizeMB<<L" MB.\n\n";
    }
    else {
        std::wcout<<L"Invalid argument combination. Usage: "<< argv[0]<<L" <sourcePath> <targetPartitionPath>[--usedefault | <threads> <blockSizeMB>]\n\n";
        return 1;
    }

    std::wcout << "Make Sure if the provided Source Path has a valid snapshot!\n\n";
    std::wcout << "[Critical] Make sure if the provided target drive is an empty drive or else it might corrupt the provided drive.\n\n";
    std::wcout << "Enter 1 to proceed and 0 to exit\n";
    bool proceed = false;
    std::cin >> proceed;
    if (!proceed)
    {
        return 0;
    }
    std::wcout << "\nSince proceed command is given from user input we're proceeding with the backup.\n\n";

    //Configure Logger
    LogUtils& logger = LogUtils::GetInstance();
    logger.Initialize();

    LOG_DEBUG(L"Inside Main\n");
    BlockCopier copier;

    // Initialize the copier with paths and parameters
    if (!copier.Initialize(srcPath, dstPath, numThreads, blockSizeMB)) {
        LOG_ERROR(L"Failed to initialize BlockCopier.\n");
        logger.DeInitialize();
        return 1; // Initialization failed
    }
    else
    {
        LOG_DEBUG(L"Main: BlockCopier class initialized successfully.\n");
    }

    // Start the copying process
    if (!copier.StartCopy()) {
        LOG_ERROR(L"Main : StartCopy method failed with error code: %d\n",GetLastError());
        logger.DeInitialize();
        return 1; 
    }
    else
    {
        LOG_DEBUG(L"Main: StartCopyMethod Succeeded.\n");
    }
    LOG_DEBUG(L"End of Main\n");
    logger.DeInitialize();
    return 0; 
}