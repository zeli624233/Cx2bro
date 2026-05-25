#pragma once
#include <windows.h>
#include <string>
#include <unordered_set>
#include <cstdint>

/// <summary>
/// Session 状态
/// </summary>
enum class SessStatus
{
    None,       // 没有任何旧 session
    Running,    // 上次未完成（游戏崩溃或用户关闭）
    Completed,  // 上次已完成
    Unknown,    // 无法判断
};

/// <summary>
/// Session 信息
/// </summary>
struct SessionInfo
{
    SessStatus status = SessStatus::None;
    int dirCount = 0;
    int fileCount = 0;
    uint64_t dirOffset = 0;
    uint64_t fileOffset = 0;
    std::wstring startTime;
    std::wstring lastUpdate;
    std::wstring lastExitReason;
};

/// <summary>
/// Session 管理器
/// </summary>
class SessionManager
{
public:
    explicit SessionManager(const std::wstring& workDir);

    bool HasPreviousSession() const;
    SessionInfo LoadSession();
    void SaveSession(const SessionInfo& info);
    void WriteCrashLog(DWORD exitCode, int dirNew, int fileNew);

    std::wstring SessionDir() const { return sessionDir_; }
    std::wstring SessionIniPath() const { return sessionIni_; }

private:
    std::wstring workDir_;
    std::wstring sessionDir_;
    std::wstring sessionIni_;
    std::wstring crashLogPath_;

    static std::wstring ReadIniValue(const std::wstring& iniPath, const std::wstring& section, const std::wstring& key);
    static void WriteIniValue(const std::wstring& iniPath, const std::wstring& section, const std::wstring& key, const std::wstring& value);
    static std::wstring CurrentTimestamp();
};
