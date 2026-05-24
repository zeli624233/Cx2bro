#include "DynamicLauncherLite.h"

#include <windows.h>
#include <detours.h>

namespace
{
    std::wstring CombinePath(const std::wstring& left, const std::wstring& right)
    {
        if (left.empty())
        {
            return right;
        }

        if (right.empty())
        {
            return left;
        }

        wchar_t tail = left.back();
        if (tail == L'\\' || tail == L'/')
        {
            return left + right;
        }

        return left + L"\\" + right;
    }

    std::wstring DirectoryName(const std::wstring& path)
    {
        size_t pos = path.find_last_of(L"\\/");
        return pos == std::wstring::npos ? L"" : path.substr(0, pos);
    }

    bool FileExists(const std::wstring& path)
    {
        DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    std::string WideToAcp(const std::wstring& value)
    {
        if (value.empty())
        {
            return "";
        }

        int length = WideCharToMultiByte(CP_ACP, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string output(length ? length - 1 : 0, '\0');
        if (length > 1)
        {
            WideCharToMultiByte(CP_ACP, 0, value.c_str(), -1, output.data(), length, nullptr, nullptr);
        }
        return output;
    }
}

bool DynamicLauncherLite::Launch(const std::wstring& moduleDirectory,
                                 const std::wstring& gameExePath,
                                 const std::wstring& moduleFileName,
                                 const std::wstring& outputRoot,
                                 const std::map<std::wstring, std::wstring>& extraEnvironment,
                                 bool waitForExit,
                                 Result& result,
                                 std::wstring& errorMessage) const
{
    if (gameExePath.empty() || !FileExists(gameExePath))
    {
        errorMessage = L"游戏主程序不存在。";
        return false;
    }

    std::wstring modulePath = CombinePath(moduleDirectory, moduleFileName);
    if (!FileExists(modulePath))
    {
        errorMessage = L"找不到注入模块：" + modulePath;
        return false;
    }

    // Detours 的 DLL 参数是窄字符串；这里沿用旧版做法转成本机 ACP。
    std::string modulePathA = WideToAcp(modulePath);
    std::wstring gameDirectory = DirectoryName(gameExePath);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};

    if (!outputRoot.empty())
    {
        CreateDirectoryW(outputRoot.c_str(), nullptr);
        SetEnvironmentVariableW(L"CXDEC_OUTPUT_ROOT", outputRoot.c_str());
    }
    for (const auto& item : extraEnvironment)
    {
        SetEnvironmentVariableW(item.first.c_str(), item.second.c_str());
    }

    if (!DetourCreateProcessWithDllW(gameExePath.c_str(),
                                     nullptr,
                                     nullptr,
                                     nullptr,
                                     FALSE,
                                     0u,
                                     nullptr,
                                     gameDirectory.c_str(),
                                     &startup,
                                     &process,
                                     modulePathA.c_str(),
                                     nullptr))
    {
        if (!outputRoot.empty())
        {
            SetEnvironmentVariableW(L"CXDEC_OUTPUT_ROOT", nullptr);
        }
        for (const auto& item : extraEnvironment)
        {
            SetEnvironmentVariableW(item.first.c_str(), nullptr);
        }
        errorMessage = L"创建游戏进程失败。";
        return false;
    }

    if (!outputRoot.empty())
    {
        SetEnvironmentVariableW(L"CXDEC_OUTPUT_ROOT", nullptr);
    }
    for (const auto& item : extraEnvironment)
    {
        SetEnvironmentVariableW(item.first.c_str(), nullptr);
    }

    result.modulePath = modulePath;
    result.processId = process.dwProcessId;

    if (waitForExit)
    {
        WaitForSingleObject(process.hProcess, INFINITE);
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}
