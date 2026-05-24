#pragma once
#include <string>
#include <vector>
#include <shellapi.h>

/// <summary>
/// 启动模式
/// </summary>
enum class LaunchMode
{
    Ask,    // 弹窗询问：继续上次 / 从头开始
    Resume, // 直接继续上次
    Fresh,  // 直接从头开始
};

/// <summary>
/// 命令行参数配置
/// </summary>
struct AppConfig
{
    std::wstring gameExe;       // --game  游戏 exe 路径（必填）
    std::wstring workDir;       // --workdir 工作区路径（必填）
    std::wstring modulePath;    // --module CxdecStringDumper.dll 路径（可选）
    std::wstring coreCliPath;   // --corecli CxdecCoreCLI.exe 路径（可选）
    LaunchMode mode = LaunchMode::Ask; // --mode ask / resume / fresh

    /// <summary>
    /// 从命令行参数解析配置
    /// </summary>
    static AppConfig Parse(int argc, wchar_t* argv[])
    {
        AppConfig config;

        for (int i = 1; i < argc; ++i)
        {
            std::wstring arg = argv[i];

            if (arg == L"--game" && i + 1 < argc)
                config.gameExe = argv[++i];
            else if (arg == L"--workdir" && i + 1 < argc)
                config.workDir = argv[++i];
            else if (arg == L"--module" && i + 1 < argc)
                config.modulePath = argv[++i];
            else if (arg == L"--corecli" && i + 1 < argc)
                config.coreCliPath = argv[++i];
            else if (arg == L"--mode" && i + 1 < argc)
            {
                std::wstring m = argv[++i];
                if (m == L"resume")
                    config.mode = LaunchMode::Resume;
                else if (m == L"fresh")
                    config.mode = LaunchMode::Fresh;
                else
                    config.mode = LaunchMode::Ask;
            }
        }

        return config;
    }

    /// <summary>
    /// 校验必填参数是否齐全
    /// </summary>
    bool IsValid() const
    {
        return !gameExe.empty() && !workDir.empty();
    }

    /// <summary>
    /// 生成错误消息
    /// </summary>
    std::wstring GetError() const
    {
        if (gameExe.empty() && workDir.empty())
            return L"缺少 --game 和 --workdir 参数";
        if (gameExe.empty())
            return L"缺少 --game 参数";
        if (workDir.empty())
            return L"缺少 --workdir 参数";
        return L"";
    }
};
