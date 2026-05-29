#include "CollectorWindow.h"
#include <commctrl.h>
#include <cstdio>

// ========== 布局常量 ==========

static constexpr int MARGIN = 10;
static constexpr int SPACING = 6;
static constexpr int LABEL_HEIGHT = 20;
static constexpr int PROGRESS_BAR_HEIGHT = 18;
static constexpr int BUTTON_HEIGHT = 28;
static constexpr int SECTION_HEADER = 18;
static constexpr int LOG_MIN_HEIGHT = 80;
static constexpr int SECTION_GAP = 6;
static constexpr int SECTION_PAD = 4;   // 鍖哄煙涓婃柟鐣欑櫧

// ========== 动态布局计算 ==========

/// <summary>
/// 鏍规嵁绐楀彛鍙敤瀹藉害璁＄畻鍚勫垎鍖轰綅缃?/// </summary>
struct Layout
{
    int width = 0;
    int height = 0;

    // 区域标题
    int titleInfoY = 0;       // "■ 状态信息"
    int statRow1Y = 0;        // 游戏路径
    int statRow2Y = 0;        // 工作区
    int statRow3Y = 0;        // 状态 / PID / 运行时间

    // 分隔线1
    int sep1Y = 0;

    // 区域标题 + 进度
    int titleProgressY = 0;   // "■ 收集进度"
    int progressY = 0;
    int progressLabelW = 0;
    int progressBarX = 0;
    int progressBarW = 0;
    int progressTextX = 0;

    // 分隔线2 + 文件大小监控
    int sep2Y = 0;
    int fileMonY = 0;

    // 鏃ュ織
    int logY = 0;
    int logHeaderH = 18;
    int logBoxH = 0;
    int logColW = 0;

    // 鎸夐挳
    int btnY = 0;
    int btnAreaW = 0;
};

static Layout CalculateLayout(int clientW, int clientH)
{
    Layout lay;
    lay.width = clientW;
    lay.height = clientH;

    int usableW = clientW - MARGIN * 2;

    // === 区域标题 + 状态信息 ===
    lay.titleInfoY = MARGIN;
    int infoStart = lay.titleInfoY + SECTION_HEADER + SECTION_PAD;
    lay.statRow1Y = infoStart;
    lay.statRow2Y = infoStart + LABEL_HEIGHT + 2;
    lay.statRow3Y = infoStart + (LABEL_HEIGHT + 2) * 2;

    // === 分隔线1 ===
    int infoSectionH = SECTION_HEADER + SECTION_PAD + LABEL_HEIGHT * 3 + SPACING * 2;
    lay.sep1Y = MARGIN + infoSectionH + SPACING;

    // === 区域标题 + 收集进度 ===
    int afterSep1 = lay.sep1Y + 4 + SECTION_PAD;
    lay.titleProgressY = afterSep1;
    lay.progressY = afterSep1 + SECTION_HEADER + SECTION_PAD;

    // 进度条布局
    lay.progressLabelW = (int)(usableW * 0.22);
    int barAndTextW = usableW - lay.progressLabelW;
    lay.progressBarW = (int)(barAndTextW * 0.55);
    lay.progressTextX = MARGIN + lay.progressLabelW + lay.progressBarW + SPACING;
    lay.progressBarX = MARGIN + lay.progressLabelW + SPACING;

    // === 文件大小监控（'进度'和'日志'之间）===
    int progressSectionH = SECTION_HEADER + SECTION_PAD + SECTION_HEADER * 4 + SPACING * 3 + SECTION_GAP;
    int afterProgress = afterSep1 + progressSectionH + SPACING;
    // 文件大小监控占一行，分隔线在其下方
    int fileMonY = afterProgress;
    lay.sep2Y = afterProgress + SECTION_HEADER + SPACING + 4;
    lay.fileMonY = fileMonY;

    // === 日志（全宽，无左栏）===
    lay.logHeaderH = 18;
    // logColW 改为全宽（不再双栏分割）
    lay.logColW = usableW;

    int btnBarH = BUTTON_HEIGHT + MARGIN;
    int logTop = lay.sep2Y + 4;
    lay.logY = logTop;
    lay.logBoxH = clientH - logTop - lay.logHeaderH - btnBarH - MARGIN * 2;
    if (lay.logBoxH < LOG_MIN_HEIGHT)
        lay.logBoxH = LOG_MIN_HEIGHT;

    // === 按钮 ===
    lay.btnY = lay.logY + lay.logHeaderH + lay.logBoxH + SPACING;
    lay.btnAreaW = usableW;

    return lay;
}

// ========== 鍒涘缓瀛愭帶浠?==========

void CreateChildControls(HWND hwnd, const std::wstring& gameExe, const std::wstring& workDir)
{
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
    HFONT hFont = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    HFONT hBoldFont = CreateFontW(-13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    HFONT hLogFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    HFONT hLogHeaderFont = CreateFontW(-14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

    RECT rc{};
    GetClientRect(hwnd, &rc);
    Layout lay = CalculateLayout(rc.right, rc.bottom);

    auto makeLabel = [&](int id, const wchar_t* text, int x, int y, int w, int h) -> HWND
    {
        HWND ctrl = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                                    x, y, w, h, hwnd, (HMENU)(INT_PTR)id, hInst, nullptr);
        SendMessageW(ctrl, WM_SETFONT, (WPARAM)hFont, TRUE);
        return ctrl;
    };

    auto makeButton = [&](int id, const wchar_t* text, int x, int y, int w, int h) -> HWND
    {
        HWND ctrl = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    x, y, w, h, hwnd, (HMENU)(INT_PTR)id, hInst, nullptr);
        SendMessageW(ctrl, WM_SETFONT, (WPARAM)hFont, TRUE);
        return ctrl;
    };

    auto makeProgressBar = [&](int id, int x, int y, int w, int h) -> HWND
    {
        HWND ctrl = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                     x, y, w, h, hwnd, (HMENU)(INT_PTR)id, hInst, nullptr);
        SendMessageW(ctrl, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        SendMessageW(ctrl, PBM_SETPOS, 0, 0);
        return ctrl;
    };

    auto makeSectionTitle = [&](const wchar_t* text, int x, int y, int w) -> HWND
    {
        HWND ctrl = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                                    x, y, w, SECTION_HEADER, hwnd, nullptr, hInst, nullptr);
        SendMessageW(ctrl, WM_SETFONT, (WPARAM)hBoldFont, TRUE);
        return ctrl;
    };

    auto makeSeparator = [&](int x, int y, int w) -> HWND
    {
        return CreateWindowExW(0, L"STATIC", nullptr, WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                                x, y, w, 2, hwnd, nullptr, hInst, nullptr);
    };

    wchar_t buf[1024];
    int usableW = rc.right - MARGIN * 2;

    // ===== 区域标题 + 状态信息 =====
    makeSectionTitle(L"状态信息", MARGIN, lay.titleInfoY, usableW);

    swprintf_s(buf, L"游戏：%ls", gameExe.c_str());
    HWND hGame = CreateWindowExW(0, L"STATIC", buf, WS_CHILD | WS_VISIBLE,
                                 MARGIN, lay.statRow1Y, usableW, LABEL_HEIGHT,
                                 hwnd, (HMENU)(INT_PTR)IDC_STAT_GAME, hInst, nullptr);
    SendMessageW(hGame, WM_SETFONT, (WPARAM)hFont, TRUE);

    swprintf_s(buf, L"工作区：%ls", workDir.c_str());
    HWND hWork = CreateWindowExW(0, L"STATIC", buf, WS_CHILD | WS_VISIBLE,
                                 MARGIN, lay.statRow2Y, usableW, LABEL_HEIGHT,
                                 hwnd, (HMENU)(INT_PTR)IDC_STAT_WORKDIR, hInst, nullptr);
    SendMessageW(hWork, WM_SETFONT, (WPARAM)hFont, TRUE);

    // 第3行：状态 | PID | 运行时间
    int col3W = (usableW - SPACING * 2) / 3;
    makeLabel(IDC_STAT_STATUS, L"状态：等待启动", MARGIN, lay.statRow3Y, col3W, LABEL_HEIGHT);
    makeLabel(IDC_STAT_PID, L"PID：--", MARGIN + col3W + SPACING, lay.statRow3Y, col3W, LABEL_HEIGHT);
    makeLabel(IDC_STAT_RUNTIME, L"运行时间：00:00:00", MARGIN + (col3W + SPACING) * 2, lay.statRow3Y, col3W, LABEL_HEIGHT);

    // ===== 分隔线1 =====
    makeSeparator(MARGIN, lay.sep1Y, usableW);

    // ===== 区域标题 + 收集进度 =====
    makeSectionTitle(L"收集进度", MARGIN, lay.titleProgressY, usableW);

    // DirectoryHash 杩涘害
    HWND hDirLabel = CreateWindowExW(0, L"STATIC", L"DirectoryHash", WS_CHILD | WS_VISIBLE,
                                     MARGIN, lay.progressY, lay.progressLabelW, SECTION_HEADER,
                                     hwnd, nullptr, hInst, nullptr);
    SendMessageW(hDirLabel, WM_SETFONT, (WPARAM)hBoldFont, TRUE);

    makeProgressBar(IDC_PROGRESS_DIR, lay.progressBarX, lay.progressY + 1, lay.progressBarW, PROGRESS_BAR_HEIGHT);

    makeLabel(IDC_TEXT_DIR, L"0 / 0  0%", lay.progressTextX, lay.progressY, 200, SECTION_HEADER);

    // FileNameHash 杩涘害
    int fileY = lay.progressY + SECTION_HEADER + SPACING;
    HWND hFileLabel = CreateWindowExW(0, L"STATIC", L"FileNameHash", WS_CHILD | WS_VISIBLE,
                                      MARGIN, fileY, lay.progressLabelW, SECTION_HEADER,
                                      hwnd, nullptr, hInst, nullptr);
    SendMessageW(hFileLabel, WM_SETFONT, (WPARAM)hBoldFont, TRUE);

    makeProgressBar(IDC_PROGRESS_FILE, lay.progressBarX, fileY + 1, lay.progressBarW, PROGRESS_BAR_HEIGHT);

    makeLabel(IDC_TEXT_FILE, L"0 / 0  0%", lay.progressTextX, fileY, 200, SECTION_HEADER);

    // 总体进度 + 本轮新增
    int overallY = fileY + SECTION_HEADER + SPACING;
    HFONT hOverallFont = CreateFontW(-14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    HWND hOverall = makeLabel(IDC_TEXT_OVERALL, L"总体进度：0.0%  本轮新增：Dir +0  File +0",
                              MARGIN, overallY, usableW, SECTION_HEADER);
    SendMessageW(hOverall, WM_SETFONT, (WPARAM)hOverallFont, TRUE);

    // ===== 加速复选框（总体进度与文件大小监控之间）=====
    int checkY = overallY + SECTION_HEADER + SPACING;
    HWND hSpeedCheck = CreateWindowExW(0, L"BUTTON", L"游戏加速（关闭声音，加速画面，10X倍速）",
                                        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                        MARGIN, checkY, usableW, SECTION_HEADER,
                                        hwnd, (HMENU)(INT_PTR)IDC_CHECK_SPEEDHACK,
                                        hInst, nullptr);
    SendMessageW(hSpeedCheck, WM_SETFONT, (WPARAM)hBoldFont, TRUE);
    SendMessageW(hSpeedCheck, BM_SETCHECK, BST_UNCHECKED, 0); // 默认不勾

    // ===== 文件大小监控（进度和日志之间）=====
    HWND hFileSize = makeLabel(IDC_TEXT_FILESIZE,
        L"日志大小（预计）：Dir.log -- MB / -- MB  |  File.log -- MB / -- MB  |  增速：-- KB/s",
        MARGIN, lay.fileMonY, usableW, SECTION_HEADER);
    SendMessageW(hFileSize, WM_SETFONT, (WPARAM)hBoldFont, TRUE);

    // ===== 分隔线2 =====
    makeSeparator(MARGIN, lay.sep2Y, usableW);

    // ===== 日志标题行：标签 + DirectoryHash 按钮（相邻同行）=====
    // "FileNameHash 实时日志" 标签（左对齐），留空间给按钮
    int titleFileNameW = 200; // "FileNameHash 实时日志" 宽（-14 粗体）
    HWND hLogTitle = CreateWindowExW(0, L"STATIC", L"FileNameHash 实时日志", WS_CHILD | WS_VISIBLE,
                    MARGIN, lay.logY, titleFileNameW, lay.logHeaderH,
                    hwnd, nullptr, hInst, nullptr);
    SendMessageW(hLogTitle, WM_SETFONT, (WPARAM)hLogHeaderFont, TRUE);

    // "[DirectoryHash 实时日志]" 按钮紧跟在标题右侧
    int btnDirW = 175;
    int btnDirX = MARGIN + titleFileNameW + SPACING;
    HWND hDirBtn = CreateWindowExW(0, L"BUTTON", L"DirectoryHash 实时日志",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    btnDirX, lay.logY, btnDirW, lay.logHeaderH,
                    hwnd, (HMENU)(INT_PTR)IDC_BTN_DIR_HASH, hInst, nullptr);
    SendMessageW(hDirBtn, WM_SETFONT, (WPARAM)hLogHeaderFont, TRUE);

    // ===== FileNameHash 实时日志框（全宽）=====
    CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOSEL | LBS_DISABLENOSCROLL,
                    MARGIN, lay.logY + lay.logHeaderH, lay.logColW, lay.logBoxH,
                    hwnd, (HMENU)(INT_PTR)IDC_LOG_FILE, hInst, nullptr);
    SendMessageW(GetDlgItem(hwnd, IDC_LOG_FILE), WM_SETFONT, (WPARAM)hLogFont, TRUE);

    // ===== 底部按钮栏 =====
    static const int BTN_COUNT = 5;
    static const int BTN_PERCENT[BTN_COUNT] = { 20, 18, 20, 22, 20 };
    static const wchar_t* BTN_TEXTS[BTN_COUNT] = {
        L"启动游戏", L"关闭游戏", L"清空目录文件", L"打开输出目录", L"返回主界面"
    };
    static const int BTN_IDS[BTN_COUNT] = {
        IDC_BTN_LAUNCH, IDC_BTN_KILLGAME, IDC_BTN_FRESH,
        IDC_BTN_OPEN_OUTPUT, IDC_BTN_EXIT
    };

    int btnAreaW = usableW;
    int totalPct = 0;
    for (int i = 0; i < BTN_COUNT; i++)
        totalPct += BTN_PERCENT[i];

    int curX = MARGIN;
    for (int i = 0; i < BTN_COUNT; i++)
    {
        int bw = (btnAreaW * BTN_PERCENT[i]) / totalPct;
        if (i < BTN_COUNT - 1)
            bw -= 2;
        makeButton(BTN_IDS[i], BTN_TEXTS[i], curX, lay.btnY, bw, BUTTON_HEIGHT);
        curX += bw + 2;
    }
}

// ========== WM_SIZE 甯冨眬鍒锋柊 ==========

void LayoutChildControls(HWND hwnd, int width, int height)
{
    Layout lay = CalculateLayout(width, height);

    HWND hLogFile = GetDlgItem(hwnd, IDC_LOG_FILE);

    if (hLogFile)
        SetWindowPos(hLogFile, nullptr, MARGIN, lay.logY + lay.logHeaderH,
                     lay.logColW, lay.logBoxH, SWP_NOZORDER);

    // 重定位 DirectoryHash 按钮（紧跟在 FileNameHash 标题右侧）
    HWND hDirBtn = GetDlgItem(hwnd, IDC_BTN_DIR_HASH);
    if (hDirBtn)
    {
        int titleFileNameW = 200;
        int btnDirW = 175;
        SetWindowPos(hDirBtn, nullptr, MARGIN + titleFileNameW + SPACING,
                     lay.logY, btnDirW, lay.logHeaderH, SWP_NOZORDER);
    }

    static const int BTN_COUNT = 5;
    static const int BTN_PERCENT[BTN_COUNT] = { 20, 18, 20, 22, 20 };
    static const int BTN_IDS[BTN_COUNT] = {
        IDC_BTN_LAUNCH, IDC_BTN_KILLGAME, IDC_BTN_FRESH,
        IDC_BTN_OPEN_OUTPUT, IDC_BTN_EXIT
    };

    int btnAreaW = width - MARGIN * 2;
    int totalPct = 0;
    for (int i = 0; i < BTN_COUNT; i++) totalPct += BTN_PERCENT[i];

    int curX = MARGIN;
    for (int i = 0; i < BTN_COUNT; i++)
    {
        int bw = (btnAreaW * BTN_PERCENT[i]) / totalPct;
        if (i < BTN_COUNT - 1) bw -= 2;
        HWND hBtn = GetDlgItem(hwnd, BTN_IDS[i]);
        if (hBtn)
            SetWindowPos(hBtn, nullptr, curX, lay.btnY, bw, BUTTON_HEIGHT, SWP_NOZORDER);
        curX += bw + 2;
    }
}

// ========== 鏇存柊 UI ==========

void UpdateProgressDisplay(HWND hwnd, int dirCount, int dirTotal, int fileCount, int fileTotal,
                           int restoredCount, int totalRestoreFiles,
                           const wchar_t* statusText)
{
    wchar_t buf[256];

    if (dirTotal > 0)
    {
        int pct = (dirCount * 100) / dirTotal;
        SendMessageW(GetDlgItem(hwnd, IDC_PROGRESS_DIR), PBM_SETPOS, pct, 0);
        swprintf_s(buf, L"%d / %d  %d%%", dirCount, dirTotal, pct);
    }
    else
    {
        SendMessageW(GetDlgItem(hwnd, IDC_PROGRESS_DIR), PBM_SETPOS, 0, 0);
        swprintf_s(buf, L"%d / 未知", dirCount);
    }
    SetWindowTextW(GetDlgItem(hwnd, IDC_TEXT_DIR), buf);

    if (fileTotal > 0)
    {
        int pct = (fileCount * 100) / fileTotal;
        SendMessageW(GetDlgItem(hwnd, IDC_PROGRESS_FILE), PBM_SETPOS, pct, 0);
        swprintf_s(buf, L"%d / %d  %d%%", fileCount, fileTotal, pct);
    }
    else
    {
        SendMessageW(GetDlgItem(hwnd, IDC_PROGRESS_FILE), PBM_SETPOS, 0, 0);
        swprintf_s(buf, L"%d / 未知", fileCount);
    }
    SetWindowTextW(GetDlgItem(hwnd, IDC_TEXT_FILE), buf);

    // 总体还原率（Dir hash + File hash 都匹配才算还原成功）
    if (totalRestoreFiles > 0)
    {
        int pct = (restoredCount * 100) / totalRestoreFiles;
        swprintf_s(buf, L"资源还原率：%.1f%%（%d / %d）",
                   (double)restoredCount * 100.0 / totalRestoreFiles,
                   restoredCount, totalRestoreFiles);
    }
    else
    {
        swprintf_s(buf, L"资源还原率：未知");
    }
    SetWindowTextW(GetDlgItem(hwnd, IDC_TEXT_OVERALL), buf);

    SetWindowTextW(GetDlgItem(hwnd, IDC_STAT_STATUS), statusText);
}

// ========== 日志写入（LISTBOX 版本） ==========
// LISTBOX 最大行数，超过时删除前半部分
static constexpr int MAX_LOG_LINES = 5000;

/// <summary>
/// 杩囨护鎺夊彲鑳藉湪 LISTBOX 涓鑷存樉绀洪敊涔辩殑鎺у埗瀛楃
/// 鍖呮嫭鎵€鏈?C0 鎺у埗瀛楃(0x00-0x1F)銆丏EL(0x7F)鍜?C1 鎺у埗瀛楃(0x80-0x9F)
/// </summary>
static bool SanitizeText(wchar_t* buf)
{
    if (!buf || !*buf) return false;

    wchar_t* write = buf;
    bool hadContent = false;
    for (wchar_t* read = buf; *read; read++)
    {
        wchar_t c = *read;
        // 璺宠繃鎵€鏈夌被鍨嬬殑鎺у埗瀛楃
        if (c == L'\r' || c == L'\n' || c == L'\t' ||
            (c > 0x00 && c < 0x20 && c != 0x09) || // 闄ab澶栫殑C0
            c == 0x7F || // DEL
            (c >= 0x80 && c <= 0x9F)) // C1鎺у埗瀛楃
        {
            continue; // 璺宠繃
        }
        *write++ = c;
        if (c > L' ') hadContent = true;
    }
    *write = L'\0';
    return hadContent;
}

/// <summary>
/// <summary>
/// 向日志框追加一行（LISTBOX 版本：LB_ADDSTRING）
/// </summary>
void AppendLogText(HWND hwnd, int controlId, const wchar_t* text)
{
    HWND hList = GetDlgItem(hwnd, controlId);
    if (!hList || !text) return;

    // 因为要过滤控制字符，需要拷贝一份
    size_t len = wcslen(text);
    wchar_t* clean = new wchar_t[len + 1];
    wcscpy_s(clean, len + 1, text);
    if (!SanitizeText(clean) || !*clean)
    {
        delete[] clean;
        return; // 过滤后变空行则跳过
    }

    // 行数太多时删除前半部分
    int count = (int)SendMessageW(hList, LB_GETCOUNT, 0, 0);
    if (count > MAX_LOG_LINES)
    {
        for (int i = 0; i < MAX_LOG_LINES / 4; i++)
            SendMessageW(hList, LB_DELETESTRING, 0, 0);
        count -= MAX_LOG_LINES / 4;
    }

    // 追加一行并滚动到底部
    int idx = (int)SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)clean);
    if (idx >= 0)
        SendMessageW(hList, LB_SETTOPINDEX, idx, 0);

    // 强制刷新，防止跨进程 WM_COPYDATA 后 UI 不更新
    InvalidateRect(hList, nullptr, TRUE);

    delete[] clean;
}

void SetStatusText(HWND hwnd, int controlId, const wchar_t* text)
{
    HWND hCtrl = GetDlgItem(hwnd, controlId);
    if (hCtrl)
        SetWindowTextW(hCtrl, text);
}

void SetRuntimeText(HWND hwnd, const wchar_t* text)
{
    HWND hCtrl = GetDlgItem(hwnd, IDC_STAT_RUNTIME);
    if (hCtrl)
        SetWindowTextW(hCtrl, text);
}

void ClearLogText(HWND hwnd, int controlId)
{
    HWND hList = GetDlgItem(hwnd, controlId);
    if (hList)
        SendMessageW(hList, LB_RESETCONTENT, 0, 0);
}
