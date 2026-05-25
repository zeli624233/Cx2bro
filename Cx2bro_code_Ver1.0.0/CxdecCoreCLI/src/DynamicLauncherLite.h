#pragma once

#include <map>
#include <string>

class DynamicLauncherLite
{
public:
    struct Result
    {
        std::wstring modulePath;
        unsigned long processId = 0;
    };

    // 使用 Detours 创建游戏进程，并把指定 DLL 注入到游戏进程里。
    bool Launch(const std::wstring& moduleDirectory,
                const std::wstring& gameExePath,
                const std::wstring& moduleFileName,
                const std::wstring& outputRoot,
                const std::map<std::wstring, std::wstring>& extraEnvironment,
                bool waitForExit,
                Result& result,
                std::wstring& errorMessage) const;
};
