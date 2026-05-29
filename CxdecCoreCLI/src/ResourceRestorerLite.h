#pragma once

#include <string>
#include <vector>

enum class RestoreUiTaskState : unsigned int
{
    Queued = 0,
    Restoring = 1,
    Completed = 2,
    Failed = 3
};

struct RestoreProgressInfo
{
    std::wstring packageName;
    RestoreUiTaskState state = RestoreUiTaskState::Queued;
    unsigned int current = 0;
    unsigned int total = 0;
    std::wstring detail;
};

using RestoreProgressCallback = void (*)(const RestoreProgressInfo& info, void* context);

class ResourceRestorerLite
{
public:
    struct PackageResult
    {
        std::wstring packageName;
        unsigned int totalFiles = 0;
        unsigned int restoredFiles = 0;
    };

    struct Result
    {
        unsigned int totalFiles = 0;
        unsigned int restoredFiles = 0;
        unsigned int missingDirectoryHash = 0;
        unsigned int missingFileNameHash = 0;
        unsigned int copyFailed = 0;
        std::wstring reportPath;
        std::vector<PackageResult> packages;
        unsigned int fallbackRestoredFiles = 0;    // traditional mode fallback restored count
        unsigned int inferenceRestoredFiles = 0;   // secondary inference restored count
    };

    // 从一个工作区还原资源名。
    // 工作区结构：
    //   Extractor_Output
    //   StaticHash_Output
    //   StringHashDumper_Output
    //   Restored_Extractor_Output
    bool RestoreWorkspace(const std::wstring& workspace,
                          Result& result,
                          std::wstring& errorMessage);

    bool RestoreWorkspaceEx(const std::wstring& workspace,
                            unsigned int workerCount,
                            bool enableFallback,
                            Result& result,
                            std::wstring& errorMessage,
                            RestoreProgressCallback progressCallback,
                            void* progressContext);

    // Secondary inference: analyze restored filenames to infer naming patterns
    // and recover additional hash-named files. Returns count of newly restored.
    static int SecondaryInference(const std::wstring& workspace, unsigned int workerCount);
};
