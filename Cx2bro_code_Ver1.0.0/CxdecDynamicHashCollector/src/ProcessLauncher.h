#pragma once
#include <string>
#include <windows.h>

/// <summary>
/// 进程启动器
/// 负责调用 CxdecCoreCLI 启动游戏并注入收集模块
/// </summary>
class ProcessLauncher
{
public:
    /// <summary>
    /// 启动动态 Hash 收集进程
    /// </summary>
    /// <param name="coreCliPath">CxdecCoreCLI.exe 路径</param>
    /// <param name="gameExe">游戏 exe 路径</param>
    /// <param name="outputRoot">User\3 工作区路径</param>
    /// <param name="modulePath">CxdecStringDumper.dll 路径（可选）</param>
    /// <returns>成功返回 true，失败返回 false</returns>
    bool LaunchDynamicDump(const std::wstring& coreCliPath,
                           const std::wstring& gameExe,
                           const std::wstring& outputRoot,
                           const std::wstring& modulePath = L"");

    /// <summary>
    /// 获取子进程 PID（启动成功后有效）
    /// </summary>
    DWORD ChildPid() const { return childPid_; }

    /// <summary>
    /// 获取子进程句柄（启动成功后有效）
    /// </summary>
    HANDLE ChildProcess() const { return childProcess_; }

    /// <summary>
    /// 获取最后一次启动失败的错误码
    /// </summary>
    DWORD LastError() const { return lastError_; }

    /// <summary>
    /// 分离子进程（不再管理生命周期）
    /// </summary>
    void Detach();

    /// <summary>
    /// 强制终止子进程（用于退出时清理）
    /// </summary>
    void Terminate();

private:
    HANDLE childProcess_ = nullptr;
    HANDLE childThread_ = nullptr;
    DWORD childPid_ = 0;
    DWORD lastError_ = 0;
};
