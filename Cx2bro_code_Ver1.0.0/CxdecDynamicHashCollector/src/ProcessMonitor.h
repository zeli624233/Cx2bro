#pragma once
#include <windows.h>
#include <string>

/// <summary>
/// 进程监控器
/// 监控游戏进程的生命周期
/// </summary>
class ProcessMonitor
{
public:
    ProcessMonitor() = default;
    ~ProcessMonitor();

    /// <summary>
    /// 附加到指定 PID 的进程
    /// </summary>
    bool AttachByPid(DWORD pid);

    /// <summary>
    /// 检查进程是否仍在运行
    /// </summary>
    bool IsRunning() const;

    /// <summary>
    /// 获取进程退出码（进程退出后有效）
    /// </summary>
    DWORD ExitCode() const;

    /// <summary>
    /// 关闭句柄
    /// </summary>
    void Close();

    /// <summary>
    /// 判断退出码是否表示崩溃（exit code != 0）
    /// </summary>
    static bool IsCrash(DWORD exitCode)
    {
        return exitCode != 0 && exitCode != STILL_ACTIVE;
    }

    /// <summary>
    /// 获取退出码的描述文本
    /// </summary>
    static std::wstring ExitCodeDescription(DWORD exitCode);

    /// <summary>
    /// 通过可执行文件名查找进程 PID
    /// 例如 FindProcessByName(L"otomeki_ckr.exe")
    /// </summary>
    /// <param name="exeName">可执行文件名（含 .exe，不区分大小写）</param>
    /// <returns>找到的第一个匹配进程 PID，未找到返回 0</returns>
    static DWORD FindProcessByName(const std::wstring& exeName);

    /// <summary>
    /// 杀死所有指定 exe 名称的进程
    /// 用于启动游戏前清理残留进程
    /// </summary>
    /// <param name="exeName">可执行文件名（含 .exe，不区分大小写）</param>
    static void KillAllByName(const std::wstring& exeName);

private:
    HANDLE processHandle_ = nullptr;
    DWORD pid_ = 0;
};
