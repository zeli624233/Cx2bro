#include "ProcessMonitor.h"
#include <tlhelp32.h>
#include <cstdio>

ProcessMonitor::~ProcessMonitor()
{
    Close();
}

bool ProcessMonitor::AttachByPid(DWORD pid)
{
    Close();
    processHandle_ = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                 FALSE, pid);
    if (!processHandle_)
        return false;
    pid_ = pid;
    return true;
}

bool ProcessMonitor::IsRunning() const
{
    if (!processHandle_)
        return false;
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(processHandle_, &exitCode))
        return false;
    return exitCode == STILL_ACTIVE;
}

DWORD ProcessMonitor::ExitCode() const
{
    if (!processHandle_)
        return (DWORD)-1;
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(processHandle_, &exitCode))
        return (DWORD)-1;
    return exitCode;
}

void ProcessMonitor::Close()
{
    if (processHandle_)
    {
        CloseHandle(processHandle_);
        processHandle_ = nullptr;
    }
    pid_ = 0;
}

DWORD ProcessMonitor::FindProcessByName(const std::wstring& exeName)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);

    DWORD foundPid = 0;
    if (Process32FirstW(snapshot, &pe))
    {
        do
        {
            // 不区分大小写比较
            if (_wcsicmp(pe.szExeFile, exeName.c_str()) == 0)
            {
                foundPid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &pe));
    }

    CloseHandle(snapshot);
    return foundPid;
}

std::wstring ProcessMonitor::ExitCodeDescription(DWORD exitCode)
{
    if (exitCode == 0)
        return L"正常退出";
    if (exitCode == STILL_ACTIVE)
        return L"运行中";
    wchar_t buf[64];
    swprintf_s(buf, L"异常退出 (0x%08X)", exitCode);
    return buf;
}

void ProcessMonitor::KillAllByName(const std::wstring& exeName)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(snapshot, &pe))
    {
        do
        {
            if (_wcsicmp(pe.szExeFile, exeName.c_str()) == 0)
            {
                HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (hProcess)
                {
                    TerminateProcess(hProcess, 1);
                    WaitForSingleObject(hProcess, 3000);
                    CloseHandle(hProcess);
                }
            }
        } while (Process32NextW(snapshot, &pe));
    }

    CloseHandle(snapshot);
}
