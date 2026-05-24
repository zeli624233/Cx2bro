#include "SessionManager.h"
#include <windows.h>
#include <shlwapi.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <ctime>
#include <iomanip>

#pragma comment(lib, "shlwapi.lib")

SessionManager::SessionManager(const std::wstring& workDir)
    : workDir_(workDir)
{
    sessionDir_ = workDir + L"\\StringHashDumper_Output";
    sessionIni_ = sessionDir_ + L"\\session.ini";
    crashLogPath_ = sessionDir_ + L"\\crash_history.log";

    // 确保目录存在
    CreateDirectoryW(sessionDir_.c_str(), nullptr);
}

bool SessionManager::HasPreviousSession() const
{
    DWORD attr = GetFileAttributesW(sessionIni_.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

SessionInfo SessionManager::LoadSession()
{
    SessionInfo info;

    if (!HasPreviousSession())
        return info;

    std::wstring statusStr = ReadIniValue(sessionIni_, L"Session", L"Status");
    if (statusStr == L"running")
        info.status = SessStatus::Running;
    else if (statusStr == L"completed")
        info.status = SessStatus::Completed;
    else
        info.status = SessStatus::Unknown;

    info.dirCount = _wtoi(ReadIniValue(sessionIni_, L"Session", L"DirectoryUniqueCount").c_str());
    info.fileCount = _wtoi(ReadIniValue(sessionIni_, L"Session", L"FileNameUniqueCount").c_str());
    info.dirOffset = _wtoi64(ReadIniValue(sessionIni_, L"Session", L"DirectoryOffset").c_str());
    info.fileOffset = _wtoi64(ReadIniValue(sessionIni_, L"Session", L"FileNameOffset").c_str());
    info.startTime = ReadIniValue(sessionIni_, L"Session", L"StartTime");
    info.lastUpdate = ReadIniValue(sessionIni_, L"Session", L"LastUpdate");
    info.lastExitReason = ReadIniValue(sessionIni_, L"Session", L"LastExitReason");

    return info;
}

void SessionManager::SaveSession(const SessionInfo& info)
{
    WriteIniValue(sessionIni_, L"Game", L"ExePath", L"");   // 由调用者维护
    WriteIniValue(sessionIni_, L"Game", L"WorkDir", workDir_);

    std::wstring statusStr;
    switch (info.status)
    {
    case SessStatus::Running: statusStr = L"running"; break;
    case SessStatus::Completed: statusStr = L"completed"; break;
    default: statusStr = L"unknown"; break;
    }

    WriteIniValue(sessionIni_, L"Session", L"Status", statusStr);
    WriteIniValue(sessionIni_, L"Session", L"StartTime", info.startTime.empty() ? CurrentTimestamp() : info.startTime);
    WriteIniValue(sessionIni_, L"Session", L"LastUpdate", CurrentTimestamp());
    WriteIniValue(sessionIni_, L"Session", L"LastExitReason", info.lastExitReason);

    wchar_t buf[64];
    swprintf_s(buf, L"%d", info.dirCount);
    WriteIniValue(sessionIni_, L"Session", L"DirectoryUniqueCount", buf);
    swprintf_s(buf, L"%d", info.fileCount);
    WriteIniValue(sessionIni_, L"Session", L"FileNameUniqueCount", buf);
    swprintf_s(buf, L"%llu", info.dirOffset);
    WriteIniValue(sessionIni_, L"Session", L"DirectoryOffset", buf);
    swprintf_s(buf, L"%llu", info.fileOffset);
    WriteIniValue(sessionIni_, L"Session", L"FileNameOffset", buf);
}

void SessionManager::WriteCrashLog(DWORD exitCode, int dirNew, int fileNew)
{
    std::wstring timestamp = CurrentTimestamp();
    wchar_t buf[512];
    swprintf_s(buf, L"[%ls] ExitCode=0x%08X Dir+%d File+%d\r\n",
               timestamp.c_str(), exitCode, dirNew, fileNew);

    HANDLE hFile = CreateFileW(crashLogPath_.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return;

    DWORD written = 0;
    std::string utf8;
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (utf8Len > 0)
    {
        utf8.resize(utf8Len);
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, (LPSTR)utf8.data(), utf8Len, nullptr, nullptr);
        WriteFile(hFile, utf8.data(), (DWORD)utf8.size() - 1, &written, nullptr);
    }
    CloseHandle(hFile);
}

// ========== 静态辅助方法 ==========

std::wstring SessionManager::ReadIniValue(const std::wstring& iniPath, const std::wstring& section, const std::wstring& key)
{
    wchar_t buffer[256] = {};
    GetPrivateProfileStringW(section.c_str(), key.c_str(), L"", buffer, 256, iniPath.c_str());
    return buffer;
}

void SessionManager::WriteIniValue(const std::wstring& iniPath, const std::wstring& section, const std::wstring& key, const std::wstring& value)
{
    WritePrivateProfileStringW(section.c_str(), key.c_str(), value.c_str(), iniPath.c_str());
}

std::wstring SessionManager::CurrentTimestamp()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[32];
    swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d:%02d",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}
