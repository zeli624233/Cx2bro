#include "ProcessLauncher.h"
#include <vector>
#include <string>

bool ProcessLauncher::LaunchDynamicDump(const std::wstring& coreCliPath,
                                        const std::wstring& gameExe,
                                        const std::wstring& outputRoot,
                                        const std::wstring& modulePath)
{
    std::wstring cmd = L"\"" + coreCliPath + L"\" --mode dynamic-stringhash --game \"" + gameExe +
                       L"\" --output-root \"" + outputRoot + L"\"";

    // 如果指定了模块 DLL，传给 CLI
    if (!modulePath.empty())
        cmd += L" --module \"" + modulePath + L"\"";

    std::wstring cmdLine = cmd;
    std::vector<wchar_t> cmdBuffer(cmdLine.begin(), cmdLine.end());
    cmdBuffer.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    BOOL result = CreateProcessW(
        coreCliPath.c_str(),
        cmdBuffer.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi
    );

    if (!result)
    {
        lastError_ = GetLastError();
        return false;
    }

    lastError_ = 0;

    childProcess_ = pi.hProcess;
    childThread_ = pi.hThread;
    childPid_ = pi.dwProcessId;

    if (childThread_)
    {
        CloseHandle(childThread_);
        childThread_ = nullptr;
    }

    return true;
}

void ProcessLauncher::Detach()
{
    if (childProcess_)
    {
        CloseHandle(childProcess_);
        childProcess_ = nullptr;
    }
    childPid_ = 0;
}

void ProcessLauncher::Terminate()
{
    if (childProcess_)
    {
        TerminateProcess(childProcess_, 1);
        WaitForSingleObject(childProcess_, 5000);
        CloseHandle(childProcess_);
        childProcess_ = nullptr;
    }
    childPid_ = 0;
}
