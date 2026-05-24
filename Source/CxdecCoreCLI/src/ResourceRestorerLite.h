#pragma once

#include <string>

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
    struct Result
    {
        unsigned int totalFiles = 0;
        unsigned int restoredFiles = 0;
        unsigned int missingDirectoryHash = 0;
        unsigned int missingFileNameHash = 0;
        unsigned int copyFailed = 0;
        std::wstring reportPath;
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
                            Result& result,
                            std::wstring& errorMessage,
                            RestoreProgressCallback progressCallback,
                            void* progressContext);
};
