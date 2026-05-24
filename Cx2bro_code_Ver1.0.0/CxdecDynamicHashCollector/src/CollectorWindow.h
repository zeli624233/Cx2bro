#pragma once
#include <windows.h>
#include <string>
#include <vector>

// 控件 ID
#define IDC_BTN_FRESH       1002
#define IDC_BTN_OPEN_OUTPUT 1004
#define IDC_BTN_STEP3       1005
#define IDC_BTN_EXIT        1006
#define IDC_BTN_LAUNCH      1007
#define IDC_BTN_KILLGAME    1008
#define IDC_BTN_DIR_HASH    1009

// 状态标签 ID
#define IDC_STAT_GAME       2001
#define IDC_STAT_WORKDIR    2002
#define IDC_STAT_STATUS     2003
#define IDC_STAT_PID        2004
#define IDC_STAT_RUNTIME    2005

// 进度条 ID
#define IDC_PROGRESS_DIR    3001
#define IDC_PROGRESS_FILE   3002

// 进度文字 ID
#define IDC_TEXT_DIR        3101
#define IDC_TEXT_FILE       3102
#define IDC_TEXT_OVERALL    3103
#define IDC_TEXT_NEW        3104

// 文件大小监控文字 ID
#define IDC_TEXT_FILESIZE   3201

// 日志框 ID
#define IDC_LOG_DIR         4001
#define IDC_LOG_FILE        4002

/// <summary>
/// 创建主窗口的所有子控件
/// </summary>
void CreateChildControls(HWND hwnd, const std::wstring& gameExe, const std::wstring& workDir);

/// <summary>
/// 调整子控件布局（WM_SIZE 时调用）
/// </summary>
void LayoutChildControls(HWND hwnd, int width, int height);

/// <summary>
/// 更新进度显示
/// </summary>
void UpdateProgressDisplay(HWND hwnd, int dirCount, int dirTotal, int fileCount, int fileTotal,
                           int restoredCount, int totalRestoreFiles,
                           const wchar_t* statusText);

/// <summary>
/// 向日志框追加文本
/// </summary>
void AppendLogText(HWND hwnd, int controlId, const wchar_t* text);

/// <summary>
/// 设置状态文本
/// </summary>
void SetStatusText(HWND hwnd, int controlId, const wchar_t* text);

/// <summary>
/// 设置运行时间
/// </summary>
void SetRuntimeText(HWND hwnd, const wchar_t* text);

/// <summary>
/// 清空日志框
/// </summary>
void ClearLogText(HWND hwnd, int controlId);
