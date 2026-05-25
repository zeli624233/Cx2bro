#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <commctrl.h>
#include <shellapi.h>
#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include "AppConfig.h"
#include "CollectorWindow.h"
#include "HashTailer.h"
#include "HashStats.h"
#include "SessionManager.h"
#include "ProcessLauncher.h"
#include "ProcessMonitor.h"

#include <string>
#include <vector>
#include <fstream>
#include <unordered_set>
#include <unordered_map>
#include <cmath>

// ========== 对话框风格常量（与主程序统一） ==========
// 使用 MB_SETFOREGROUND | MB_TASKMODAL 确保弹窗在前台不被遮挡
#define DLG_STYLE (MB_SETFOREGROUND | MB_TASKMODAL)

// ========== 全局上下文 ==========

struct CollectorContext
{
    AppConfig config;
    HWND hwnd = nullptr;

    // 模块
    SessionManager* session = nullptr;
    HashTailer* dirTailer = nullptr;
    HashTailer* fileTailer = nullptr;
    HashStats* stats = nullptr;
    ProcessLauncher* launcher = nullptr;
    ProcessMonitor* monitor = nullptr;

    // 统计状态
    int dirTotal = 0;       // 预期总数（非空，从 ExpectedHashNeed 读取）
    int fileTotal = 0;
    int prevDirCount = 0;   // 上次统计时的数量
    int prevFileCount = 0;

    // 运行时
    ULONGLONG startTick = 0;
    bool running = false;   // 收集是否激活
    bool gameLaunching = false; // 是否正在启动游戏
    bool gameRunning = false;   // 游戏进程是否运行中
    DWORD gamePid = 0;      // 游戏进程 PID

    std::wstring logDir;    // StringHashDumper_Output 路径

    // 连续重复抑制：记录上次显示的日志行
    std::wstring lastDirLine;   // 上次显示的 DirectoryHash 日志行
    std::wstring lastFileLine;  // 上次显示的 FileNameHash 日志行

    bool warnNoExtractor = false; // 无 Extractor_Output 需提示

    // Dumper 监控
    ULONGLONG lastDataTick = 0;     // 上次收到 dumper 数据的时间
    bool dumperWarningShown = false; // 是否已弹过 dumper 无响应警告

    // DirectoryHash 弹窗
    std::vector<std::wstring> dirLogBuffer;  // 缓存的目录 hash 行
    HWND hDirPopup = nullptr;                // 弹窗句柄

    // 体积比移动平均（用 File 反推 Dir）
    double dirFileRatioMA = 0.0;        // Dir/File 体积比移动平均
    int ratioSampleCount = 0;           // 已采样的比值数
    static constexpr int RATIO_BUFFER_MAX = 6;
    double ratioSamples[6] = {0.0};     // 环形缓冲区(最近6个=约9秒)
    int ratioWriteIdx = 0;              // 写入位置

    // 增速跟踪（实时增量，最近 1.5s 的变化）
    double prevTotalKB = 0.0;
    ULONGLONG prevTick = 0;
};

AppConfig g_config;

// Dir.log 的 hash→目录名映射（用于和 Extractor_Output 的目录 hash 比对）
static std::unordered_map<std::wstring, std::wstring> g_dirHashToName;

// Extractor_Output 中的目录 hash 集合（目录名本身就是 hash 值）
static std::unordered_set<std::wstring> g_extractorDirHashes;

// FileNameHash.log 的 hash→文件名映射
static std::unordered_map<std::wstring, std::wstring> g_fileHashToName;
// Extractor_Output 中的文件 hash 集合
static std::unordered_set<std::wstring> g_extractorFileHashes;
// 文件 hash → 所属目录 hash 映射（用于精确还原率计算）
static std::unordered_map<std::wstring, std::wstring> g_fileToDirHash;

// ========== 调试日志（写入文件，供 AI 远程诊断） ==========
static void DebugLog(const CollectorContext& ctx, const wchar_t* msg)
{
    std::wstring path = ctx.logDir + L"\\debug_collector.log";
    HANDLE hFile = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t prefix[32];
    swprintf_s(prefix, L"[%02d:%02d:%02d.%03d] ",
               st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    std::wstring line = std::wstring(prefix) + msg + L"\r\n";
    DWORD written = 0;
    WriteFile(hFile, line.c_str(), (DWORD)line.size() * sizeof(wchar_t), &written, nullptr);
    CloseHandle(hFile);
}

constexpr wchar_t WINDOW_CLASS_NAME[] = L"CxdecHashCollectorClass";
constexpr wchar_t DIR_POPUP_CLASS_NAME[] = L"CxdecHashDirPopupClass";
constexpr wchar_t WINDOW_TITLE[] = L"Cx2bro 动态 hash模块 控制台";
constexpr int MIN_WIDTH = 820;
constexpr int MIN_HEIGHT = 580;
constexpr UINT_PTR TIMER_ID = 1;
constexpr int TIMER_INTERVAL_MS = 1500;
constexpr UINT_PTR TIMER_RUNTIME_ID = 2;
constexpr int TIMER_RUNTIME_INTERVAL_MS = 1000;
constexpr UINT WM_APP_INIT_SESSION = WM_APP + 0x100;
constexpr UINT WM_APP_SHOW_NO_OUTPUT_WARNING = WM_APP + 0x101;

// ========== 前向声明 ==========

LRESULT CALLBACK MainWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK DirPopupProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
void OnCreate(HWND hwnd, CollectorContext& ctx);
void OnDestroy(HWND hwnd, CollectorContext& ctx);
void OnSize(HWND hwnd, CollectorContext& ctx);
void OnTimerTick(HWND hwnd, CollectorContext& ctx);
void OnCommand(HWND hwnd, WPARAM wParam, CollectorContext& ctx);

void InitSession(HWND hwnd, CollectorContext& ctx);
void StartWithExisting(HWND hwnd, CollectorContext& ctx);
void StartCollection(HWND hwnd, CollectorContext& ctx, bool fresh);
void UpdateUi(const CollectorContext& ctx);
void PollLogsForStats(CollectorContext& ctx);

/// <summary>
/// 处理 WM_COPYDATA（独立函数避免与 SEH 冲突）
/// </summary>
static void HandleCopyData(HWND hwnd, CollectorContext* ctx, PCOPYDATASTRUCT pcds)
{
    if (!pcds || !pcds->lpData || pcds->cbData <= 0 || !ctx || !ctx->stats)
        return;

    const wchar_t* line = (const wchar_t*)pcds->lpData;
    if (!line || !*line) return;

    // 记录 Dumper 最后通信时间
    ctx->lastDataTick = GetTickCount64();
    ctx->dumperWarningShown = false;

    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0) return;

    std::string utf8Line(utf8Len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line, -1, &utf8Line[0], utf8Len, nullptr, nullptr);
    if (!utf8Line.empty() && utf8Line.back() == '\0')
        utf8Line.pop_back();

    bool isDir = (pcds->dwData == 0);

    if (isDir)
    {
        // === DirectoryHash：更新 hash→名称映射 ===
        // 格式: "dirname##YSig##hash"
        std::wstring lineStr(line);
        size_t splitPos = lineStr.find(L"##YSig##");
        if (splitPos != std::wstring::npos)
        {
            std::wstring dirName = lineStr.substr(0, splitPos);
            std::wstring hashHex = lineStr.substr(splitPos + 8);
            g_dirHashToName[hashHex] = dirName;

            ctx->stats->AddDirectoryLine(utf8Line);

            // 连续重复抑制：只缓冲非重复的新行
            if (ctx->lastDirLine != line)
            {
                ctx->dirLogBuffer.push_back(line);
                ctx->lastDirLine = line;

                // 如果弹窗开着，直接发过去
                if (ctx->hDirPopup && IsWindow(ctx->hDirPopup))
                {
                    HWND hList = GetDlgItem(ctx->hDirPopup, 4001);
                    if (hList)
                    {
                        // 清除空行，保持整洁
                        if (wcsstr(line, L"%EmptyString%") != nullptr)
                            line = L"<EmptyString>";
                        SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)line);
                        int lastIdx = (int)SendMessageW(hList, LB_GETCOUNT, 0, 0) - 1;
                        if (lastIdx >= 0)
                            SendMessageW(hList, LB_SETTOPINDEX, lastIdx, 0);
                    }
                }
            }
        }
    }
    else
    {
        // === FileNameHash：构建 hash→文件名映射 ===
        std::wstring lineStr(line);
        size_t splitPos = lineStr.find(L"##YSig##");
        if (splitPos != std::wstring::npos)
        {
            std::wstring fileName = lineStr.substr(0, splitPos);
            std::wstring hashHex = lineStr.substr(splitPos + 8);
            g_fileHashToName[hashHex] = fileName;

            ctx->stats->AddFileNameLine(utf8Line);

            if (ctx->lastFileLine != line)
            {
                AppendLogText(hwnd, IDC_LOG_FILE, line);
                ctx->lastFileLine = line;
            }
        }
    }
}

/// <summary>
/// 主消息循环（带 SEH 保护，防止意外闪退）
/// </summary>
static void RunMessageLoop()
{
    for (;;)
    {
        MSG msg{};
        __try
        {
            while (GetMessageW(&msg, nullptr, 0, 0))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            break; // WM_QUIT → 正常退出
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            DWORD excCode = GetExceptionCode();
            wchar_t buf[160];
            swprintf_s(buf, 160, L"消息循环异常 (0x%08X)\n程序已恢复，可继续操作。\n\n请将异常码反馈给开发者。", excCode);
            MessageBoxW(nullptr, buf, L"Cxdec 动态 Hash 收集器 - 异常",
                        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
            // 继续外层循环，重新进入消息循环
        }
    }
}

// ========== WinMain ==========

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, wchar_t*, int nCmdShow)
{
    AppConfig config;
    {
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv)
        {
            config = AppConfig::Parse(argc, argv);
            LocalFree(argv);
        }
    }
    g_config = config;

    if (!g_config.IsValid())
    {
        MessageBoxW(nullptr, g_config.GetError().c_str(), L"参数错误", MB_OK | MB_ICONWARNING | DLG_STYLE);
        return 1;
    }

    INITCOMMONCONTROLSEX icex{};
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    // 加载项目图标（从 exe 所在目录的上级找 app_icon.ico）
    HICON hAppIcon = nullptr;
    HICON hAppIconSm = nullptr;
    {
        wchar_t selfPath[MAX_PATH];
        GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
        std::wstring iconPath(selfPath);
        size_t pos = iconPath.find_last_of(L"\\/");
        if (pos != std::wstring::npos)
            iconPath = iconPath.substr(0, pos + 1) + L"..\\app_icon.ico";
        else
            iconPath = L"app_icon.ico";

        hAppIcon = (HICON)LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON,
                                      32, 32, LR_LOADFROMFILE);
        hAppIconSm = (HICON)LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON,
                                        16, 16, LR_LOADFROMFILE);
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainWindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = WINDOW_CLASS_NAME;
    if (hAppIcon) wc.hIcon = hAppIcon;
    if (hAppIconSm) wc.hIconSm = hAppIconSm;
    RegisterClassExW(&wc);

    // 注册 DirectoryHash 弹窗类
    WNDCLASSEXW popupWc{};
    popupWc.cbSize = sizeof(popupWc);
    popupWc.style = CS_HREDRAW | CS_VREDRAW;
    popupWc.lpfnWndProc = DirPopupProc;
    popupWc.hInstance = hInstance;
    popupWc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    popupWc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    popupWc.lpszClassName = DIR_POPUP_CLASS_NAME;
    if (hAppIcon) popupWc.hIcon = hAppIcon;
    if (hAppIconSm) popupWc.hIconSm = hAppIconSm;
    RegisterClassExW(&popupWc);

    HWND hwnd = CreateWindowExW(0, WINDOW_CLASS_NAME, WINDOW_TITLE,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 840, 620,
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) return 1;

    // 居中
    {
        RECT rc{};
        GetWindowRect(hwnd, &rc);
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        SetWindowPos(hwnd, nullptr,
            (sw - (rc.right - rc.left)) / 2,
            (sh - (rc.bottom - rc.top)) / 2,
            0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // 把自己的窗口句柄写入环境变量，dumper 启动时读取
    wchar_t hwndStr[32];
    swprintf_s(hwndStr, L"%llu", (ULONGLONG)(ULONG_PTR)hwnd);
    SetEnvironmentVariableW(L"CXDEC_COLLECTOR_HWND", hwndStr);

    RunMessageLoop();

    return 0;
}

// ========== DirectoryHash 弹窗窗口过程 ==========

constexpr wchar_t DIR_POPUP_LISTBOX_CLASS[] = L"ListBox";

LRESULT CALLBACK DirPopupProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        // 创建一个覆盖整个客户区的列表框
        RECT rc;
        GetClientRect(hwnd, &rc);
        HWND hList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOSEL | LBS_DISABLENOSCROLL,
            0, 0, rc.right, rc.bottom,
            hwnd, (HMENU)(INT_PTR)4001,
            ((LPCREATESTRUCT)lParam)->hInstance, nullptr);
        // 设置 Microsoft YaHei UI 字体，和主窗口一致
        HFONT hFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        if (hList && hFont)
            SendMessageW(hList, WM_SETFONT, (WPARAM)hFont, TRUE);
        return 0;
    }
    case WM_SIZE:
    {
        HWND hList = GetDlgItem(hwnd, 4001);
        if (hList)
            SetWindowPos(hList, nullptr, 0, 0, LOWORD(lParam), HIWORD(lParam), SWP_NOZORDER);
        return 0;
    }
    case WM_DESTROY:
        // 通知主窗口清空弹窗句柄
        EnumWindows([](HWND h, LPARAM) -> BOOL {
            wchar_t cls[64];
            GetClassNameW(h, cls, 64);
            if (wcscmp(cls, WINDOW_CLASS_NAME) == 0)
            {
                CollectorContext* ctx = (CollectorContext*)GetWindowLongPtrW(h, GWLP_USERDATA);
                if (ctx) ctx->hDirPopup = nullptr;
                return FALSE;
            }
            return TRUE;
        }, 0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ========== 窗口过程 ==========

LRESULT CALLBACK MainWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    CollectorContext* ctx = (CollectorContext*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg)
    {
    case WM_CREATE:
    {
        CollectorContext* pCtx = new CollectorContext();
        pCtx->hwnd = hwnd;
        pCtx->config = g_config;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)pCtx);
        OnCreate(hwnd, *pCtx);
        return 0;
    }
    case WM_DESTROY:
        if (ctx) OnDestroy(hwnd, *ctx);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        delete ctx;
        PostQuitMessage(0);
        return 0;

    case WM_SIZE:
        if (ctx) OnSize(hwnd, *ctx);
        return 0;

    case WM_TIMER:
        if (ctx)
        {
            __try
            {
                if (wParam == TIMER_RUNTIME_ID)
                {
                    // 每秒刷新运行时间（仅游戏运行中）
                    if (ctx->startTick > 0 && ctx->gameRunning)
                    {
                        ULONGLONG elapsed = (GetTickCount64() - ctx->startTick) / 1000;
                        wchar_t buf[64];
                        swprintf_s(buf, 64, L"运行时间：%02llu:%02llu:%02llu",
                                   elapsed / 3600, (elapsed % 3600) / 60, elapsed % 60);
                        SetRuntimeText(ctx->hwnd, buf);
                    }
                    else
                    {
                        // 游戏未运行，显示空闲
                        SetRuntimeText(ctx->hwnd, L"运行时间：--:--:--");
                    }
                }
                else
                {
                    OnTimerTick(hwnd, *ctx);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                // 定时器异常捕获——防止闪退
                AppendLogText(hwnd, IDC_LOG_DIR, L"[系统] 定时器异常，已恢复");
                AppendLogText(hwnd, IDC_LOG_FILE, L"[系统] 定时器异常，已恢复");
            }
        }
        return 0;

    case WM_APP_INIT_SESSION:
        if (ctx)
        {
            InitSession(hwnd, *ctx);
            SetTimer(hwnd, TIMER_ID, TIMER_INTERVAL_MS, nullptr);
            SetTimer(hwnd, TIMER_RUNTIME_ID, TIMER_RUNTIME_INTERVAL_MS, nullptr);

            // 无 Extractor_Output 提醒（窗口显示后再弹）
            if (ctx->warnNoExtractor)
            {
                PostMessageW(hwnd, WM_APP_SHOW_NO_OUTPUT_WARNING, 0, 0);
            }
        }
        return 0;

    case WM_APP_SHOW_NO_OUTPUT_WARNING:
        if (ctx)
        {
            MessageBoxW(hwnd,
                L"未检测到已提取的 XP3 资源目录，无法推算预期 Hash 数量。\n"
                L"建议先点击「步骤1：提取 XP3」后再打开本收集器。\n"
                L"不影响：您仍可先启动游戏开始收集，只是无法显示百分比。",
                L"提示", MB_OK | MB_ICONINFORMATION | DLG_STYLE);
        }
        return 0;

    case WM_COMMAND:
        if (ctx)
        {
            __try
            {
                OnCommand(hwnd, wParam, *ctx);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                AppendLogText(hwnd, IDC_LOG_DIR, L"[系统] 按钮处理异常，已恢复");
                AppendLogText(hwnd, IDC_LOG_FILE, L"[系统] 按钮处理异常，已恢复");
            }
        }
        return 0;

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = MIN_WIDTH;
        mmi->ptMinTrackSize.y = MIN_HEIGHT;
        return 0;
    }

    case WM_COPYDATA:
    {
        HandleCopyData(hwnd, ctx, (PCOPYDATASTRUCT)lParam);
        return TRUE;
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    {
        // 日志框白底
        HWND hCtrl = (HWND)lParam;
        int ctrlId = GetDlgCtrlID(hCtrl);
        if (ctrlId == IDC_LOG_DIR || ctrlId == IDC_LOG_FILE)
        {
            SetBkColor((HDC)wParam, RGB(255, 255, 255));
            SetTextColor((HDC)wParam, RGB(0, 0, 0));
            static HBRUSH hWhiteBrush = CreateSolidBrush(RGB(255, 255, 255));
            return (LRESULT)hWhiteBrush;
        }
        break;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ========== 递归扫描 Extractor_Output ==========

/// <summary>
/// 递归扫描目录
/// - depth=0: Extractor_Output 根
/// - depth=1: 包目录
/// - depth=2: hash 目录 → 记录 hashDir 并传给子层
/// - depth>=2 下的文件：记录到 hashFileNames 和 fileToDirMap
/// </summary>
static void ScanDirectoryTree(const std::wstring& dirPath, int& outFileCount, int& outDirCount,
                               std::unordered_set<std::wstring>* hashDirNames = nullptr,
                               std::unordered_set<std::wstring>* hashFileNames = nullptr,
                               std::unordered_map<std::wstring, std::wstring>* fileToDirMap = nullptr,
                               int depth = 0,
                               const std::wstring& currentHashDir = L"")
{
    std::wstring searchPath = dirPath + L"\\*";
    WIN32_FIND_DATAW ffd;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE)
        return;

    do {
        if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0)
            continue;

        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            outDirCount++;
            std::wstring dirName = ffd.cFileName;
            if (hashDirNames && depth >= 1)
                hashDirNames->insert(dirName);
            std::wstring subPath = dirPath + L"\\" + dirName;

            // depth=1 时下一层的目录名就是 hash 目录名，传给子层
            std::wstring nextHashDir = (depth >= 1) ? dirName : currentHashDir;
            ScanDirectoryTree(subPath, outFileCount, outDirCount,
                              hashDirNames, hashFileNames, fileToDirMap,
                              depth + 1, nextHashDir);
        }
        else
        {
            outFileCount++;
            std::wstring fileName = ffd.cFileName;
            if (hashFileNames && depth >= 2)
                hashFileNames->insert(fileName);
            // depth >= 2 的文件：记录它属于哪个 hash 目录
            if (fileToDirMap && depth >= 2 && !currentHashDir.empty())
            {
                fileToDirMap->insert({fileName, currentHashDir});
            }
        }
    } while (FindNextFileW(hFind, &ffd) != 0);

    FindClose(hFind);
}

// ========== OnCreate ==========

void OnCreate(HWND hwnd, CollectorContext& ctx)
{
    ctx.logDir = ctx.config.workDir + L"\\StringHashDumper_Output";

    // 工作区显示路径：User\3\StringHashDumper_Output
    std::wstring displayWorkDir = ctx.logDir;

    // 创建 UI
    CreateChildControls(hwnd, ctx.config.gameExe, displayWorkDir);
    SetStatusText(hwnd, IDC_STAT_STATUS, L"状态：就绪");

    // 初始化模块
    ctx.session = new SessionManager(ctx.config.workDir);
    ctx.stats = new HashStats();
    ctx.launcher = nullptr;
    ctx.monitor = nullptr;
    ctx.startTick = GetTickCount64();

    // 初始化文件 tailer
    ctx.dirTailer = new HashTailer(ctx.logDir + L"\\DirectoryHash.log");
    ctx.fileTailer = new HashTailer(ctx.logDir + L"\\FileNameHash.log");

    // === 读取现有日志，恢复 Hash 计数 ===
    StartWithExisting(hwnd, ctx);

    // === 读取预期值：三级优先级 ===
    // 1) ExpectedHashNeed.ini（来自之前还原报告的精确数据）
    // 2) 扫描 Extractor_Output 推算（实时文件和目录数）
    // 3) 都没有 → 等待数据提示
    {
        std::wstring expectedPath = ctx.config.workDir + L"\\ExpectedHashNeed.ini";
        wchar_t buf[64];

        // 优先级1：ini 文件
        DWORD len = GetPrivateProfileStringW(L"Expected", L"DirectoryHashNeeded", L"0", buf, 64, expectedPath.c_str());
        if (len > 0) ctx.dirTotal = _wtoi(buf);
        len = GetPrivateProfileStringW(L"Expected", L"FileNameHashNeeded", L"0", buf, 64, expectedPath.c_str());
        if (len > 0) ctx.fileTotal = _wtoi(buf);

        // 优先级2：扫描 Extractor_Output（始终扫描，用于填充全局映射供 UpdateUi 计算进度）
        {
            std::wstring extractorPath = ctx.config.workDir + L"\\Extractor_Output";
            DWORD attr = GetFileAttributesW(extractorPath.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
            {
                int fileCount = 0, dirCount = 0;
                g_extractorDirHashes.clear();
                g_extractorFileHashes.clear();
                g_fileToDirHash.clear();
                ScanDirectoryTree(extractorPath, fileCount, dirCount,
                                  &g_extractorDirHashes, &g_extractorFileHashes, &g_fileToDirHash);
                // 只有 ini 没加载到预期总数时才从扫描结果取
                if (ctx.dirTotal == 0 && ctx.fileTotal == 0)
                {
                    if (fileCount > 0 || dirCount > 0)
                    {
                        ctx.dirTotal = (int)g_extractorDirHashes.size();
                        ctx.fileTotal = (int)g_extractorFileHashes.size();
                    }
                }
            }
        }

        // 优先级3：都没有 → 标记等待数据提示
        if (ctx.dirTotal == 0 && ctx.fileTotal == 0)
            ctx.warnNoExtractor = true;
    }
    // 初始化日志显示
    AppendLogText(hwnd, IDC_LOG_DIR, L"[系统] 收集器已就绪，等待操作...");
    AppendLogText(hwnd, IDC_LOG_FILE, L"[系统] 收集器已就绪，等待操作...");

    // 延迟 session 初始化，确保窗口显示后再弹 MessageBox
    PostMessageW(hwnd, WM_APP_INIT_SESSION, 0, 0);

    // 诊断：输出启动时的关键状态
    {
        wchar_t d[256];
        bool dirExists = ctx.dirTailer && ctx.dirTailer->Exists();
        bool fileExists = ctx.fileTailer && ctx.fileTailer->Exists();
        swprintf_s(d, L"[诊断] dirTotal=%d fileTotal=%d  DirHash=%zu FileHash=%zu  dirLogExist=%d fileLogExist=%d",
                   ctx.dirTotal, ctx.fileTotal,
                   ctx.stats->DirectoryCount(), ctx.stats->FileNameCount(),
                   dirExists, fileExists);
        AppendLogText(hwnd, IDC_LOG_FILE, d);
    }
}

// ========== Session 处理 ==========

void InitSession(HWND hwnd, CollectorContext& ctx)
{
    // 现在 StartWithExisting 已在 OnCreate 中同步调用
    // InitSession 仅保留给可能需要的延迟操作
}

/// <summary>
/// 读取现有日志（不存档不删除），用于首次启动时已有完整日志的情况
/// </summary>
void StartWithExisting(HWND hwnd, CollectorContext& ctx)
{
    // 清空日志框，只显示后续新增内容
    ClearLogText(hwnd, IDC_LOG_DIR);
    ClearLogText(hwnd, IDC_LOG_FILE);

    AppendLogText(hwnd, IDC_LOG_DIR, L"[系统] 发现现有日志，正在读取...");
    AppendLogText(hwnd, IDC_LOG_FILE, L"[系统] 发现现有日志，正在读取...");

    // 读取已有日志——只读一次（不 while(true) 循环，避免 dumper 持续写入导致死循环）
    // 读到的数据显示到日志框，然后 SkipToEnd，后续增量由 PollLogs 处理
    auto loadOnce = [&](HashTailer* tailer, bool isDir) -> int
    {
        if (!tailer || !tailer->Exists()) return 0;
        tailer->ResetOffset();
        auto lines = tailer->ReadNewLines();
        std::vector<std::string> newLines;
        for (const auto& line : lines)
        {
            if (isDir)
            {
                // 构建 hash→名称映射
                size_t splitPos = line.find("##YSig##");
                if (splitPos != std::string::npos)
                {
                    std::string dirName = line.substr(0, splitPos);
                    std::string hashHex = line.substr(splitPos + 8);
                    int dW = MultiByteToWideChar(CP_UTF8, 0, dirName.c_str(), (int)dirName.size(), nullptr, 0);
                    int hW = MultiByteToWideChar(CP_UTF8, 0, hashHex.c_str(), (int)hashHex.size(), nullptr, 0);
                    if (dW > 0 && hW > 0)
                    {
                        std::wstring wDir(dW, L'\0'), wHash(hW, L'\0');
                        MultiByteToWideChar(CP_UTF8, 0, dirName.c_str(), (int)dirName.size(), &wDir[0], dW);
                        MultiByteToWideChar(CP_UTF8, 0, hashHex.c_str(), (int)hashHex.size(), &wHash[0], hW);
                        g_dirHashToName[wHash] = wDir;
                    }
                }

                size_t before = ctx.stats->DirectoryCount();
                ctx.stats->AddDirectoryLine(line);
                if (ctx.stats->DirectoryCount() > before)
                {
                    newLines.push_back(line);
                }
            }
            else
            {
                // 构建文件 hash→名称映射
                size_t splitPos = line.find("##YSig##");
                if (splitPos != std::string::npos)
                {
                    std::string fileName = line.substr(0, splitPos);
                    std::string hashHex = line.substr(splitPos + 8);
                    int fW = MultiByteToWideChar(CP_UTF8, 0, fileName.c_str(), (int)fileName.size(), nullptr, 0);
                    int hW = MultiByteToWideChar(CP_UTF8, 0, hashHex.c_str(), (int)hashHex.size(), nullptr, 0);
                    if (fW > 0 && hW > 0)
                    {
                        std::wstring wFile(fW, L'\0'), wHash(hW, L'\0');
                        MultiByteToWideChar(CP_UTF8, 0, fileName.c_str(), (int)fileName.size(), &wFile[0], fW);
                        MultiByteToWideChar(CP_UTF8, 0, hashHex.c_str(), (int)hashHex.size(), &wHash[0], hW);
                        g_fileHashToName[wHash] = wFile;
                    }
                }

                size_t before = ctx.stats->FileNameCount();
                ctx.stats->AddFileNameLine(line);
                if (ctx.stats->FileNameCount() > before)
                {
                    newLines.push_back(line);
                }
            }
        }
        if (!newLines.empty())
        {
            for (const auto& l : newLines)
            {
                int wlen = MultiByteToWideChar(CP_UTF8, 0, l.c_str(), (int)l.size(), nullptr, 0);
                if (wlen > 0)
                {
                    std::wstring wl(wlen, L'\0');
                    MultiByteToWideChar(CP_UTF8, 0, l.c_str(), (int)l.size(), &wl[0], wlen);
                    AppendLogText(hwnd, isDir ? IDC_LOG_DIR : IDC_LOG_FILE, wl.c_str());
                }
            }
        }
        tailer->SkipToEnd();
        return (int)(isDir ? ctx.stats->DirectoryCount() : ctx.stats->FileNameCount());
    };

    ctx.prevDirCount = loadOnce(ctx.dirTailer, true);
    ctx.prevFileCount = loadOnce(ctx.fileTailer, false);
    ctx.running = true;
    ctx.startTick = GetTickCount64();

    wchar_t buf[128];
    swprintf_s(buf, L"已读取现有日志：Dir %d  File %d",
               ctx.prevDirCount, ctx.prevFileCount);
    SetStatusText(hwnd, IDC_STAT_STATUS, buf);
    UpdateUi(ctx);
    AppendLogText(hwnd, IDC_LOG_DIR, L"[系统] 现有日志读取完成");
    AppendLogText(hwnd, IDC_LOG_FILE, L"[系统] 现有日志读取完成");
}

void StartCollection(HWND hwnd, CollectorContext& ctx, bool fresh)
{
    ctx.startTick = GetTickCount64();

    // 清空日志框，只显示本轮新内容
    ClearLogText(hwnd, IDC_LOG_DIR);
    ClearLogText(hwnd, IDC_LOG_FILE);

    if (fresh)
    {
        // 截断日志文件（不删除，避免 Dumper 持有旧句柄写幽灵文件）
        // 截断后写回 UTF-16LE BOM，Dumper 会继续追加
        std::wstring dirLogPath = ctx.logDir + L"\\DirectoryHash.log";
        std::wstring fileLogPath = ctx.logDir + L"\\FileNameHash.log";
        std::wstring uniLogPath = ctx.logDir + L"\\Universal.log";

        auto truncateFile = [](const std::wstring& path) {
            HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hFile == INVALID_HANDLE_VALUE) return; // 文件不存在，跳过
            // 截断到 0
            SetFilePointer(hFile, 0, nullptr, FILE_BEGIN);
            SetEndOfFile(hFile);
            // 写入 UTF-16LE BOM
            unsigned char bom[] = { 0xFF, 0xFE };
            DWORD written = 0;
            WriteFile(hFile, bom, 2, &written, nullptr);
            CloseHandle(hFile);
        };
        truncateFile(dirLogPath);
        truncateFile(fileLogPath);
        truncateFile(uniLogPath);
        AppendLogText(hwnd, IDC_LOG_DIR, L"[系统] 已清空日志文件（截断）");
        AppendLogText(hwnd, IDC_LOG_FILE, L"[系统] 已清空日志文件（截断）");

        // 清空内存中的统计数据
        ctx.stats->Clear();
        g_dirHashToName.clear();
        g_fileHashToName.clear();
        g_extractorDirHashes.clear();
        g_extractorFileHashes.clear();
        g_fileToDirHash.clear();

        // 重置 tailer 偏移量（不重建，因为文件还在）
        ctx.dirTailer->ResetOffset();
        ctx.fileTailer->ResetOffset();

        ctx.prevDirCount = 0;
        ctx.prevFileCount = 0;

        // 重置体积比和每hash系数
        ctx.dirFileRatioMA = 0.0;
        ctx.ratioSampleCount = 0;
        ctx.ratioWriteIdx = 0;
        ctx.prevTotalKB = 0.0;
        ctx.prevTick = 0;

        // 重新扫描 Extractor_Output 恢复全局映射（UpdateUi 匹配需要）
        {
            std::wstring extractorPath = ctx.config.workDir + L"\\Extractor_Output";
            DWORD attr = GetFileAttributesW(extractorPath.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
            {
                int fileCount = 0, dirCount = 0;
                ScanDirectoryTree(extractorPath, fileCount, dirCount,
                                  &g_extractorDirHashes, &g_extractorFileHashes, &g_fileToDirHash);
            }
        }

        SetStatusText(hwnd, IDC_STAT_STATUS, L"状态：已从头开始（数据已清空）");
        AppendLogText(hwnd, IDC_LOG_DIR, L"[系统] 已从头开始，数据已清空");
        AppendLogText(hwnd, IDC_LOG_FILE, L"[系统] 已从头开始，数据已清空");
    }
    else
    {
        // 继续上次：从旧日志加载已有数据到去重集合
        SessionInfo info = ctx.session->LoadSession();

        // 如果日志存在，把已有内容全量读入去重集合
        if (ctx.dirTailer->Exists())
        {
            ctx.dirTailer->ResetOffset();
            auto lines = ctx.dirTailer->ReadNewLines();
            for (const auto& line : lines)
                ctx.stats->AddDirectoryLine(line);
            ctx.dirTailer->SkipToEnd(); // 后续只读新增
            ctx.prevDirCount = (int)ctx.stats->DirectoryCount();
        }
        if (ctx.fileTailer->Exists())
        {
            ctx.fileTailer->ResetOffset();
            auto lines = ctx.fileTailer->ReadNewLines();
            for (const auto& line : lines)
                ctx.stats->AddFileNameLine(line);
            ctx.fileTailer->SkipToEnd();
            ctx.prevFileCount = (int)ctx.stats->FileNameCount();
        }

        wchar_t buf[128];
        swprintf_s(buf, L"状态：已继续上次（Dir %d  File %d）", ctx.prevDirCount, ctx.prevFileCount);
        SetStatusText(hwnd, IDC_STAT_STATUS, buf);
        AppendLogText(hwnd, IDC_LOG_DIR, L"[系统] 已继续上次收集");
        AppendLogText(hwnd, IDC_LOG_FILE, L"[系统] 已继续上次收集");
    }

    ctx.running = true;
    UpdateUi(ctx);
    EnableWindow(GetDlgItem(hwnd, IDC_BTN_LAUNCH), TRUE);
}

// ========== OnTimer ==========

void OnTimerTick(HWND hwnd, CollectorContext& ctx)
{
    // === 游戏进程检测 ===
    if (ctx.gameLaunching && !ctx.gameRunning)
    {
        // 尝试通过 exe 名查找游戏进程
        std::wstring exeName = ctx.config.gameExe;
        size_t pos = exeName.find_last_of(L"\\/");
        if (pos != std::wstring::npos)
            exeName = exeName.substr(pos + 1);

        DWORD pid = ProcessMonitor::FindProcessByName(exeName);
        if (pid > 0)
        {
            // 找到进程后先验证它是否真的存活
            HANDLE hTest = OpenProcess(PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, pid);
            if (hTest)
            {
                DWORD waitResult = WaitForSingleObject(hTest, 0);
                if (waitResult == WAIT_OBJECT_0)
                {
                    // 进程已退出，清理
                    CloseHandle(hTest);
                    ctx.gameLaunching = false;
                    ctx.gamePid = 0;
                    EnableWindow(GetDlgItem(hwnd, IDC_BTN_LAUNCH), TRUE);
                    SetStatusText(hwnd, IDC_STAT_STATUS, L"状态：游戏已退出（启动后立即退出）");
                    SetStatusText(hwnd, IDC_STAT_PID, L"PID：--");
                    AppendLogText(hwnd, IDC_LOG_DIR, L"[系统] 游戏进程启动后立即退出");
                    AppendLogText(hwnd, IDC_LOG_FILE, L"[系统] 游戏进程启动后立即退出");
                }
                else
                {
                    ctx.gamePid = pid;
                    ctx.monitor = new ProcessMonitor();
                    if (ctx.monitor->AttachByPid(pid))
                    {
                        ctx.gameRunning = true;
                        ctx.gameLaunching = false;
                        SetStatusText(hwnd, IDC_STAT_STATUS, L"状态：运行中");
                        wchar_t buf[64];
                        swprintf_s(buf, L"PID：%u", pid);
                        SetStatusText(hwnd, IDC_STAT_PID, buf);
                        wchar_t logBuf[128];
                        swprintf_s(logBuf, L"[系统] 已检测到游戏进程（PID：%u）", pid);
                        AppendLogText(hwnd, IDC_LOG_DIR, logBuf);
                        AppendLogText(hwnd, IDC_LOG_FILE, logBuf);
                        EnableWindow(GetDlgItem(hwnd, IDC_BTN_LAUNCH), FALSE);
                        EnableWindow(GetDlgItem(hwnd, IDC_BTN_FRESH), FALSE);
                    }
                    else
                    {
                        delete ctx.monitor;
                        ctx.monitor = nullptr;
                        CloseHandle(hTest);
                    }
                }
            }
            else
            {
                // 无法打开进程，可能已退出或权限不足
                ctx.gameLaunching = false;
                ctx.gamePid = 0;
                EnableWindow(GetDlgItem(hwnd, IDC_BTN_LAUNCH), TRUE);
                SetStatusText(hwnd, IDC_STAT_STATUS, L"状态：游戏启动失败（无法访问进程）");
                AppendLogText(hwnd, IDC_LOG_DIR, L"[系统] 启动的游戏进程无法访问，可能已退出");
                AppendLogText(hwnd, IDC_LOG_FILE, L"[系统] 启动的游戏进程无法访问，可能已退出");
            }
        }
    }

    // 游戏运行中 → 检测是否退出
    if (ctx.gameRunning)
    {
        bool exited = false;
        DWORD exitCode = 0;
        if (ctx.monitor)
        {
            if (!ctx.monitor->IsRunning())
            {
                exited = true;
                exitCode = ctx.monitor->ExitCode();
            }
        }
        else
        {
            // monitor 为空但 gameRunning 为 true → 状态异常，兜底检测
            exited = true;
            exitCode = 0;
        }

        if (exited)
        {
            ctx.gameRunning = false;
            ctx.startTick = 0;  // 游戏退出，停止计时

            wchar_t buf[256];
            swprintf_s(buf, L"[系统] 检测到游戏进程（PID：%u）已退出（%ls）",
                       ctx.gamePid, ProcessMonitor::ExitCodeDescription(exitCode).c_str());
            AppendLogText(hwnd, IDC_LOG_DIR, buf);
            AppendLogText(hwnd, IDC_LOG_FILE, buf);

            swprintf_s(buf, L"状态：游戏已退出（%ls）", ProcessMonitor::ExitCodeDescription(exitCode).c_str());
            SetStatusText(hwnd, IDC_STAT_STATUS, buf);
            SetStatusText(hwnd, IDC_STAT_PID, L"PID：--");

            // 记录崩溃
            if (ProcessMonitor::IsCrash(exitCode))
            {
                ctx.session->WriteCrashLog(exitCode, 0, 0);
            }

            if (ctx.monitor)
            {
                ctx.monitor->Close();
                delete ctx.monitor;
                ctx.monitor = nullptr;
            }
            EnableWindow(GetDlgItem(hwnd, IDC_BTN_LAUNCH), TRUE);
            EnableWindow(GetDlgItem(hwnd, IDC_BTN_FRESH), TRUE);

            // 游戏退出警告——只有在非主动点击"关闭游戏"按钮时才弹
            //（检测到进程退出且既不是按钮 kill 也不是正常关闭时弹出）
            wchar_t warnMsg[512];
            swprintf_s(warnMsg,
                L"游戏进程已退出。\n\n"
                L"如果游戏是因为 Dumper 问题崩溃的，请检查日志。\n"
                L"Hash 收集已暂停，可重新启动游戏继续。\n\n"
                L"退出码：%lu（%ls）",
                exitCode, ProcessMonitor::ExitCodeDescription(exitCode).c_str());
            MessageBoxW(hwnd, warnMsg, L"游戏退出",
                        MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TASKMODAL);
        }
    }

    // === 从日志文件更新统计数据（不显示，只用于进度累计）===
    PollLogsForStats(ctx);

    // === Dumper 活跃度监控 ===
    if (ctx.gameRunning && ctx.running && ctx.lastDataTick > 0)
    {
        ULONGLONG now = GetTickCount64();
        ULONGLONG elapsedSinceLastData = now - ctx.lastDataTick;

        // 超过 30 秒没有收到 Dumper 数据 → 写入日志警告（只写一次）
        if (elapsedSinceLastData > 30000 && !ctx.dumperWarningShown)
        {
            ctx.dumperWarningShown = true;
            wchar_t warnBuf[128];
            swprintf_s(warnBuf, L"[系统] 警告：Dumper 超过 %llu 秒未发送数据，可能已停止工作", elapsedSinceLastData / 1000);
            AppendLogText(hwnd, IDC_LOG_DIR, warnBuf);
            AppendLogText(hwnd, IDC_LOG_FILE, warnBuf);
        }
    }

    // === 文件大小监控更新 ===
    {
        wchar_t sizeBuf[256];
        std::wstring dirLogPath = ctx.logDir + L"\\DirectoryHash.log";
        std::wstring fileLogPath = ctx.logDir + L"\\FileNameHash.log";

        auto getFileSizeMB = [](const std::wstring& path) -> double {
            WIN32_FILE_ATTRIBUTE_DATA info{};
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
                return 0.0;
            LARGE_INTEGER li;
            li.LowPart = info.nFileSizeLow;
            li.HighPart = info.nFileSizeHigh;
            return (double)li.QuadPart / (1024.0 * 1024.0);
        };
        auto getFileSizeKB = [](const std::wstring& path) -> double {
            WIN32_FILE_ATTRIBUTE_DATA info{};
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
                return 0.0;
            LARGE_INTEGER li;
            li.LowPart = info.nFileSizeLow;
            li.HighPart = info.nFileSizeHigh;
            return (double)li.QuadPart / 1024.0;
        };

        double dirMB = getFileSizeMB(dirLogPath);
        double fileMB = getFileSizeMB(fileLogPath);
        double dirKB = getFileSizeKB(dirLogPath);
        double fileKB = getFileSizeKB(fileLogPath);
        double totalKB = dirKB + fileKB;

        // === 体积比采样（DirKB / FileKB，移动平均）===
        {
            if (fileKB > 1.0)
            {
                double ratio = dirKB / fileKB;
                ctx.ratioSamples[ctx.ratioWriteIdx % 6] = ratio;
                ctx.ratioWriteIdx = (ctx.ratioWriteIdx + 1) % 6;
                if (ctx.ratioSampleCount < 6)
                    ctx.ratioSampleCount++;
                double sum = 0.0;
                for (int i = 0; i < ctx.ratioSampleCount; i++)
                    sum += ctx.ratioSamples[i];
                ctx.dirFileRatioMA = sum / ctx.ratioSampleCount;
            }
        }

        // === File 预估：比例法（当前大小 / File进度） ===
        ULONGLONG fileCount = ctx.stats->FileNameCount();
double estimatedFileMB = 0.0;
if (ctx.fileTotal > 0 && fileCount > 0)
{
    double fileProgress = min((double)fileCount / ctx.fileTotal, 1.0);
    if (fileProgress > 0.001)
        estimatedFileMB = fileMB / fileProgress;
    else
        estimatedFileMB = fileMB;
}
else
{
    estimatedFileMB = fileMB;
}

        // === Dir 预估（体积比 × File 预估）===
        double estimatedDirMB = 0.0;
        if (ctx.dirFileRatioMA > 0.01 && estimatedFileMB > 0.1)
            estimatedDirMB = ctx.dirFileRatioMA * estimatedFileMB;
        else
            estimatedDirMB = dirMB;

        // 增速信息（实时增量，最近 1.5s 的变化）
        wchar_t speedStr[32];
        double realtimeSpeedKBps = 0.0;
        if (ctx.prevTick > 0)
        {
            double dt = (double)(GetTickCount64() - ctx.prevTick) / 1000.0;
            if (dt > 0.0)
            {
                double deltaKB = totalKB - ctx.prevTotalKB;
                if (deltaKB >= 0.0)
                    realtimeSpeedKBps = deltaKB / dt;
            }
        }
        ctx.prevTotalKB = totalKB;
        ctx.prevTick = GetTickCount64();
        if (realtimeSpeedKBps > 0.0)
            swprintf_s(speedStr, L"+%.0f KB/s", realtimeSpeedKBps);
        else
            swprintf_s(speedStr, L"0 KB/s");

        swprintf_s(sizeBuf,
            L"日志大小（预计）：Dir.log %.2f MB / %.2f MB  |  File.log %.2f MB / %.2f MB  |  增速：%ls",
            dirMB, estimatedDirMB, fileMB, estimatedFileMB, speedStr);
        SetWindowTextW(GetDlgItem(hwnd, IDC_TEXT_FILESIZE), sizeBuf);
    }

    // === 更新 UI ===
    UpdateUi(ctx);

    if (!ctx.running)
    {
        return;
    }

    // 3. 保存 session

    // 3. 保存 session
    SessionInfo sinfo{};
    sinfo.status = SessStatus::Running;
    sinfo.dirCount = (int)ctx.stats->DirectoryCount();
    sinfo.fileCount = (int)ctx.stats->FileNameCount();
    sinfo.dirOffset = ctx.dirTailer->Offset();
    sinfo.fileOffset = ctx.fileTailer->Offset();
    sinfo.lastUpdate = L"auto";
    ctx.session->SaveSession(sinfo);
}

// ========== 从日志文件更新统计数据（仅统计，不显示）==========

void PollLogsForStats(CollectorContext& ctx)
{
    bool gotData = false;

    // DirectoryHash.log：构建 hash→目录名映射
    auto dirLines = ctx.dirTailer->ReadNewLines();
    for (const auto& line : dirLines)
    {
        gotData = true;
        // 格式: "dirname##YSig##hash"
        size_t splitPos = line.find("##YSig##");
        if (splitPos == std::string::npos) continue;

        std::string dirName = line.substr(0, splitPos);
        std::string hashHex = line.substr(splitPos + 8); // 跳过 ##YSig##

        // 转 UTF-16
        int dirWlen = MultiByteToWideChar(CP_UTF8, 0, dirName.c_str(), (int)dirName.size(), nullptr, 0);
        int hashWlen = MultiByteToWideChar(CP_UTF8, 0, hashHex.c_str(), (int)hashHex.size(), nullptr, 0);
        if (dirWlen <= 0 || hashWlen <= 0) continue;

        std::wstring wDirName(dirWlen, L'\0');
        std::wstring wHash(hashWlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, dirName.c_str(), (int)dirName.size(), &wDirName[0], dirWlen);
        MultiByteToWideChar(CP_UTF8, 0, hashHex.c_str(), (int)hashHex.size(), &wHash[0], hashWlen);

        // 加入 hash→名称映射
        g_dirHashToName[wHash] = wDirName;

        // 统计用
        ctx.stats->AddDirectoryLine(line);
    }

    // FileNameHash.log：构建 hash→文件名映射
    auto fileLines = ctx.fileTailer->ReadNewLines();
    for (const auto& line : fileLines)
    {
        size_t splitPos = line.find("##YSig##");
        if (splitPos == std::string::npos) continue;

        std::string fileName = line.substr(0, splitPos);
        std::string hashHex = line.substr(splitPos + 8);

        int fnWlen = MultiByteToWideChar(CP_UTF8, 0, fileName.c_str(), (int)fileName.size(), nullptr, 0);
        int fhWlen = MultiByteToWideChar(CP_UTF8, 0, hashHex.c_str(), (int)hashHex.size(), nullptr, 0);
        if (fnWlen <= 0 || fhWlen <= 0) continue;

        std::wstring wFileName(fnWlen, L'\0');
        std::wstring wHash(fhWlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, fileName.c_str(), (int)fileName.size(), &wFileName[0], fnWlen);
        MultiByteToWideChar(CP_UTF8, 0, hashHex.c_str(), (int)hashHex.size(), &wHash[0], fhWlen);

        g_fileHashToName[wHash] = wFileName;
        ctx.stats->AddFileNameLine(line);
    }

    // 从日志文件读取到新数据 → 更新 Dumper 活跃时间戳
    if (gotData)
    {
        ctx.lastDataTick = GetTickCount64();
        ctx.dumperWarningShown = false;
    }
}

// ========== UI 更新 ==========

void UpdateUi(const CollectorContext& ctx)
{
    // Dir：统计 g_extractorDirHashes 中有多少在 g_dirHashToName 里
    int matchedDirCount = 0;
    if (!g_extractorDirHashes.empty())
    {
        for (const auto& hashDir : g_extractorDirHashes)
        {
            if (g_dirHashToName.find(hashDir) != g_dirHashToName.end())
                matchedDirCount++;
        }
    }
    int dirCount = matchedDirCount;

    // File：统计 g_extractorFileHashes 中有多少在 g_fileHashToName 里
    int matchedFileCount = 0;
    if (!g_extractorFileHashes.empty())
    {
        for (const auto& hashFile : g_extractorFileHashes)
        {
            if (g_fileHashToName.find(hashFile) != g_fileHashToName.end())
                matchedFileCount++;
        }
    }
    int fileCount = matchedFileCount;

    // 精确还原率：文件 hash 和所属目录 hash 都匹配才算还原成功
    int restoredCount = 0;
    int totalRestoreFiles = (int)g_fileToDirHash.size();
    if (totalRestoreFiles > 0)
    {
        for (auto it = g_fileToDirHash.begin(); it != g_fileToDirHash.end(); ++it)
        {
            if (g_fileHashToName.find(it->first) != g_fileHashToName.end() &&
                g_dirHashToName.find(it->second) != g_dirHashToName.end())
            {
                restoredCount++;
            }
        }
    }
    // 如果还未扫描完，用近似值
    if (totalRestoreFiles == 0 && ctx.fileTotal > 0)
    {
        totalRestoreFiles = ctx.fileTotal;
        restoredCount = dirCount > 0 ? fileCount : 0; // 目录hash有匹配才可能还原
    }

    // 根据实际游戏状态显示状态文本
    std::wstring status;
    if (ctx.gameRunning)
        status = L"状态：运行中";
    else if (ctx.gameLaunching)
        status = L"状态：正在启动...";
    else
        status = L"状态：就绪";
    UpdateProgressDisplay(ctx.hwnd, dirCount, ctx.dirTotal, fileCount, ctx.fileTotal,
                          restoredCount, totalRestoreFiles, status.c_str());

    // 运行时间（仅游戏运行时有效）
    wchar_t buf[64];
    if (ctx.startTick > 0 && ctx.gameRunning)
    {
        ULONGLONG elapsed = (GetTickCount64() - ctx.startTick) / 1000;
        swprintf_s(buf, 64, L"运行时间：%02llu:%02llu:%02llu",
                   elapsed / 3600, (elapsed % 3600) / 60, elapsed % 60);
    }
    else
    {
        swprintf_s(buf, 64, L"运行时间：--:--:--");
    }
    SetRuntimeText(ctx.hwnd, buf);
}

// ========== OnCommand ==========

void OnCommand(HWND hwnd, WPARAM wParam, CollectorContext& ctx)
{
    int id = LOWORD(wParam);

    switch (id)
    {
    case IDC_BTN_LAUNCH:
    {
        // 先杀遗留进程（Python 侧优先，C++ 侧兜底清理）
        {
            std::wstring exeName = ctx.config.gameExe;
            size_t pos = exeName.find_last_of(L"\\/");
            if (pos != std::wstring::npos)
                exeName = exeName.substr(pos + 1);

            AppendLogText(hwnd, IDC_LOG_DIR, (L"[系统] 清理旧进程: " + exeName).c_str());
            AppendLogText(hwnd, IDC_LOG_FILE, (L"[系统] 清理旧进程: " + exeName).c_str());

            int killCount = 0;
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snapshot != INVALID_HANDLE_VALUE)
            {
                PROCESSENTRY32W pe{};
                pe.dwSize = sizeof(pe);
                if (Process32FirstW(snapshot, &pe))
                {
                    do
                    {
                        if (_wcsicmp(pe.szExeFile, exeName.c_str()) == 0)
                        {
                            HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                            if (hProc)
                            {
                                TerminateProcess(hProc, 1);
                                WaitForSingleObject(hProc, 2000);
                                CloseHandle(hProc);
                                killCount++;
                            }
                        }
                    } while (Process32NextW(snapshot, &pe));
                }
                CloseHandle(snapshot);
            }

            wchar_t killMsg[128];
            swprintf_s(killMsg, L"[系统] 已清理 %d 个旧进程", killCount);
            AppendLogText(hwnd, IDC_LOG_DIR, killMsg);
            AppendLogText(hwnd, IDC_LOG_FILE, killMsg);
        }

        // 查找 CxdecCoreCLI.exe，优先级：
        // 1) --corecli 参数（Python 传入）
        // 2) workDir\..\..\core\CxdecCoreCLI.exe
        // 3) 收集器自身所在目录的 CxdecCoreCLI.exe
        // 4) 系统 PATH
        std::wstring coreCli = ctx.config.coreCliPath;
        if (coreCli.empty())
        {
            std::wstring coreDir = ctx.config.workDir + L"\\..\\..\\core";
            coreCli = coreDir + L"\\CxdecCoreCLI.exe";
            DWORD attr = GetFileAttributesW(coreCli.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES)
            {
                // 试收集器自身所在目录
                wchar_t selfPath[MAX_PATH];
                GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
                std::wstring selfDir = selfPath;
                size_t pos = selfDir.find_last_of(L"\\/");
                if (pos != std::wstring::npos)
                    selfDir = selfDir.substr(0, pos);
                coreCli = selfDir + L"\\CxdecCoreCLI.exe";
                attr = GetFileAttributesW(coreCli.c_str());
                if (attr == INVALID_FILE_ATTRIBUTES)
                    coreCli = L"CxdecCoreCLI.exe"; // 让系统 PATH
            }
        }

        AppendLogText(hwnd, IDC_LOG_DIR, (L"[系统] 正在启动游戏: " + ctx.config.gameExe).c_str());
        AppendLogText(hwnd, IDC_LOG_FILE, (L"[系统] CLI: " + coreCli).c_str());

        ctx.launcher = new ProcessLauncher();
        bool ok = ctx.launcher->LaunchDynamicDump(coreCli, ctx.config.gameExe, ctx.config.workDir, ctx.config.modulePath);

        if (!ok)
        {
            DWORD errCode = ctx.launcher->LastError();
            wchar_t errMsg[128];
            swprintf_s(errMsg, L"[系统] 启动失败，错误码：%u (0x%08X)", errCode, errCode);
            AppendLogText(hwnd, IDC_LOG_DIR, errMsg);
            AppendLogText(hwnd, IDC_LOG_FILE, errMsg);
            delete ctx.launcher;
            ctx.launcher = nullptr;
            break;
        }

        ctx.gameLaunching = true;
        ctx.startTick = GetTickCount64();
        EnableWindow(GetDlgItem(hwnd, IDC_BTN_LAUNCH), FALSE);
        SetStatusText(hwnd, IDC_STAT_STATUS, L"状态：正在启动...");

        // 如果没有现有日志，自动开始收集
        if (!ctx.running)
        {
            ctx.running = true;
        }
        break;
    }

    case IDC_BTN_KILLGAME:
    {
        std::wstring exeName = ctx.config.gameExe;
        size_t pos = exeName.find_last_of(L"\\/");
        if (pos != std::wstring::npos)
            exeName = exeName.substr(pos + 1);

        if (exeName.empty())
        {
            AppendLogText(hwnd, IDC_LOG_DIR, L"[系统] 无法获取游戏文件名");
            AppendLogText(hwnd, IDC_LOG_FILE, L"[系统] 无法获取游戏文件名");
            break;
        }

        std::wstring msg = L"确定要关闭游戏进程吗？\n\n";
        msg += L"进程：" + exeName;
        if (ctx.gamePid > 0)
        {
            wchar_t pidBuf[32];
            swprintf_s(pidBuf, L"\nPID：%u", ctx.gamePid);
            msg += pidBuf;
        }

        int choice = MessageBoxW(hwnd, msg.c_str(), L"关闭游戏",
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | DLG_STYLE);
        if (choice != IDYES)
        {
            AppendLogText(hwnd, IDC_LOG_DIR, L"[系统] 已取消关闭游戏");
            AppendLogText(hwnd, IDC_LOG_FILE, L"[系统] 已取消关闭游戏");
            break;
        }

        // 使用 taskkill 杀进程（与 Python subprocess.run(["taskkill","/f","/im",exe]) 等效）
        std::wstring killCmd = L"taskkill /f /im " + exeName + L" >nul 2>&1";
        int killResult = _wsystem(killCmd.c_str());

        wchar_t killBuf[256];
        if (killResult == 0)
        {
            swprintf_s(killBuf, L"[系统] 已手动关闭游戏进程（PID：%u）：%ls", ctx.gamePid, exeName.c_str());
            SetStatusText(hwnd, IDC_STAT_STATUS, L"状态：游戏已手动关闭");
        }
        else
        {
            swprintf_s(killBuf, L"[系统] 关闭游戏失败（可能未运行）：%ls", exeName.c_str());
        }
        AppendLogText(hwnd, IDC_LOG_DIR, killBuf);
        AppendLogText(hwnd, IDC_LOG_FILE, killBuf);

        // 清理监控状态
        ctx.gameRunning = false;
        ctx.startTick = 0;
        ctx.gameLaunching = false;
        ctx.gamePid = 0;
        if (ctx.monitor)
        {
            ctx.monitor->Close();
            delete ctx.monitor;
            ctx.monitor = nullptr;
        }
        EnableWindow(GetDlgItem(hwnd, IDC_BTN_LAUNCH), TRUE);
        EnableWindow(GetDlgItem(hwnd, IDC_BTN_FRESH), TRUE);  // 游戏已关闭，启用"从头开始"
        SetStatusText(hwnd, IDC_STAT_PID, L"PID：--");
        break;
    }

    case IDC_BTN_FRESH:
        {
            int firstConfirm = MessageBoxW(hwnd,
                L"⚠ 从头开始将清空所有已收集的 Hash 数据。\n\n日志文件（DirectoryHash.log / FileNameHash.log / Universal.log）将被删除，数据不可恢复。\n\n确定要清空并从头开始吗？",
                L"警告：数据将丢失",
                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | DLG_STYLE);
            if (firstConfirm == IDYES)
            {
                StartCollection(hwnd, ctx, true);
            }
        }
        break;

    case IDC_BTN_DIR_HASH:
        {
            if (ctx.hDirPopup && IsWindow(ctx.hDirPopup))
            {
                // 已存在则激活
                ShowWindow(ctx.hDirPopup, SW_SHOWNORMAL);
                SetForegroundWindow(ctx.hDirPopup);
            }
            else
            {
                // 创建弹窗（大小与原来左栏相当：400x300）
                HWND hPopup = CreateWindowExW(WS_EX_DLGMODALFRAME,
                    DIR_POPUP_CLASS_NAME, L"DirectoryHash 的实时日志",
                    WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_SIZEBOX,
                    CW_USEDEFAULT, CW_USEDEFAULT, 420, 340,
                    hwnd, nullptr, (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE), nullptr);
                if (hPopup)
                {
                    // 居中于主窗口
                    RECT rcMain, rcPopup;
                    GetWindowRect(hwnd, &rcMain);
                    GetWindowRect(hPopup, &rcPopup);
                    int pw = rcPopup.right - rcPopup.left;
                    int ph = rcPopup.bottom - rcPopup.top;
                    SetWindowPos(hPopup, nullptr,
                        rcMain.left + (rcMain.right - rcMain.left - pw) / 2,
                        rcMain.top + (rcMain.bottom - rcMain.top - ph) / 2,
                        0, 0, SWP_NOSIZE | SWP_NOZORDER);

                    // 填充所有已有缓冲行
                    HWND hList = GetDlgItem(hPopup, 4001);
                    if (hList)
                    {
                        for (const auto& cachedLine : ctx.dirLogBuffer)
                        {
                            const wchar_t* display = cachedLine.c_str();
                            if (wcsstr(display, L"%EmptyString%") != nullptr)
                                display = L"<EmptyString>";
                            SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)display);
                        }
                        int lastIdx = (int)SendMessageW(hList, LB_GETCOUNT, 0, 0) - 1;
                        if (lastIdx >= 0)
                            SendMessageW(hList, LB_SETTOPINDEX, lastIdx, 0);
                    }

                    ctx.hDirPopup = hPopup;
                    ShowWindow(hPopup, SW_SHOWNORMAL);
                }
            }
        }
        break;

    case IDC_BTN_OPEN_OUTPUT:
        ShellExecuteW(hwnd, L"open", ctx.logDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;

    case IDC_BTN_EXIT:
        // "返回主界面"：确认后保存 session 并退出进程，让 Python 下次重新启动
        {
            int choice = MessageBoxW(hwnd,
                L"确定要返回主界面吗？\n\n"
                L"返回后动态 Hash 收集器将关闭。\n"
                L"如果游戏正在运行，Dumper 模块仍会留在游戏中。\n"
                L"下次启动收集器时会自动恢复之前的收集记录。",
                L"返回主界面",
                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | DLG_STYLE);
            if (choice != IDYES)
            {
                AppendLogText(hwnd, IDC_LOG_DIR, L"[系统] 已取消返回主界面");
                AppendLogText(hwnd, IDC_LOG_FILE, L"[系统] 已取消返回主界面");
                break;
            }

            // 保存 session
            if (ctx.running)
            {
                SessionInfo sinfo{};
                sinfo.status = SessStatus::Running;
                sinfo.dirCount = (int)ctx.stats->DirectoryCount();
                sinfo.fileCount = (int)ctx.stats->FileNameCount();
                sinfo.dirOffset = ctx.dirTailer ? ctx.dirTailer->Offset() : 0;
                sinfo.fileOffset = ctx.fileTailer ? ctx.fileTailer->Offset() : 0;
                sinfo.lastExitReason = L"user_return";
                ctx.session->SaveSession(sinfo);
            }

            // 1) 通知 Python 弹回前台（Python 端每 500ms 轮询命名事件）
            HANDLE hActivateEvent = CreateEventW(nullptr, TRUE, FALSE, L"Local\\Cx2bro_Activate");
            if (hActivateEvent)
            {
                SetEvent(hActivateEvent);
                CloseHandle(hActivateEvent);
            }
            // 2) 允许任意进程设置前台窗口（配合 Python 下文的 SetForegroundWindow 兜底）
            AllowSetForegroundWindow((DWORD)-1);

            // 3) 最终兜底：直接搜索 Python 窗口并激活
            HWND hPythonWnd = nullptr;
            EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
                wchar_t title[256];
                if (GetWindowTextW(hwnd, title, 256) > 0 && wcsstr(title, L"Cx2bro"))
                {
                    *((HWND*)lParam) = hwnd;
                    return FALSE;
                }
                return TRUE;
            }, (LPARAM)&hPythonWnd);
            if (hPythonWnd)
            {
                ShowWindow(hPythonWnd, SW_RESTORE);
                typedef void (WINAPI* SwitchToThisWindow_t)(HWND, BOOL);
                HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
                if (hUser32)
                {
                    auto pSwitch = (SwitchToThisWindow_t)GetProcAddress(hUser32, "SwitchToThisWindow");
                    if (pSwitch) pSwitch(hPythonWnd, TRUE);
                }
                SetWindowPos(hPythonWnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
                BringWindowToTop(hPythonWnd);
                SetForegroundWindow(hPythonWnd);
            }

            // 关闭自身（Python 下次会重新启动）
            DestroyWindow(hwnd);
        }
        break;
    }
}

// ========== OnDestroy / OnSize ==========

void OnDestroy(HWND hwnd, CollectorContext& ctx)
{
    KillTimer(hwnd, TIMER_ID);
    KillTimer(hwnd, TIMER_RUNTIME_ID);

    // 保存 session
    if (ctx.running)
    {
        SessionInfo sinfo{};
        sinfo.status = SessStatus::Running;
        sinfo.dirCount = (int)ctx.stats->DirectoryCount();
        sinfo.fileCount = (int)ctx.stats->FileNameCount();
        sinfo.dirOffset = ctx.dirTailer ? ctx.dirTailer->Offset() : 0;
        sinfo.fileOffset = ctx.fileTailer ? ctx.fileTailer->Offset() : 0;
        sinfo.lastExitReason = L"user_closed";
        ctx.session->SaveSession(sinfo);
    }

    // 清理
    if (ctx.launcher)
    {
        if (ctx.gameRunning)
            ctx.launcher->Terminate();
        delete ctx.launcher;
    }
    delete ctx.monitor;
    delete ctx.session;
    delete ctx.stats;
    delete ctx.dirTailer;
    delete ctx.fileTailer;
}

void OnSize(HWND hwnd, CollectorContext& ctx)
{
    (void)ctx;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    LayoutChildControls(hwnd, rc.right, rc.bottom);
}
