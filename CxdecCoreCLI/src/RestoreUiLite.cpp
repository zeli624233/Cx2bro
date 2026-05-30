#include "RestoreUiLite.h"

#include "../resource.h"
#include "ResourceRestorerLite.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

namespace
{
    constexpr UINT WM_APP_RESTORE_PROGRESS = WM_APP + 0x201u;
    constexpr UINT WM_APP_RESTORE_DONE = WM_APP + 0x202u;

    struct ProgressMessage
    {
        RestoreProgressInfo info;
    };

    struct FinishMessage
    {
        bool success = false;
        ResourceRestorerLite::Result result{};
        std::wstring errorMessage;
    };

    struct RowState
    {
        RestoreUiTaskState state = RestoreUiTaskState::Queued;
        unsigned int current = 0;
        unsigned int total = 0;
        std::wstring detail;
    };

    struct UiContext
    {
        std::wstring workspace;
        std::wstring restoredOutput;
        unsigned int workerCount = 1;
        HWND window = nullptr;
        HWND taskList = nullptr;
        HWND progressBar = nullptr;
        HWND statusText = nullptr;
        HWND outputEdit = nullptr;
        HWND workerInfo = nullptr;
        HWND logEdit = nullptr;
        HANDLE workerThread = nullptr;
        bool enableFallback = true;
        bool enableInference = true;
        bool closing = false;
        std::unordered_map<std::wstring, int> rowByPackage;
        std::unordered_map<std::wstring, RowState> stateByPackage;
        std::unordered_map<std::wstring, unsigned int> loggedPercentByPackage;
        std::chrono::steady_clock::time_point startTime;
    };

    std::wstring Combine(const std::wstring& left, const std::wstring& right)
    {
        if (left.empty()) return right;
        if (right.empty()) return left;
        if (left.back() == L'\\' || left.back() == L'/') return left + right;
        return left + L"\\" + right;
    }

    bool IsDotEntry(const wchar_t* name)
    {
        return wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0;
    }

    bool ReadUtf16File(const std::wstring& path, std::wstring& content)
    {
        HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(hFile, &size) || size.QuadPart > 0x100000)
        {
            CloseHandle(hFile);
            return false;
        }
        DWORD bytesRead = 0;
        std::vector<wchar_t> buf(size.QuadPart / sizeof(wchar_t) + 2);
        if (!ReadFile(hFile, buf.data(), (DWORD)size.QuadPart, &bytesRead, nullptr))
        {
            CloseHandle(hFile);
            return false;
        }
        CloseHandle(hFile);
        buf[bytesRead / sizeof(wchar_t)] = L'\0';
        // Skip BOM
        wchar_t* data = buf.data();
        if (bytesRead >= 2 && data[0] == 0xFEFF) data++;
        content = data;
        return !content.empty();
    }

    std::vector<std::wstring> CollectPackageNames(const std::wstring& extractorOutput)
    {
        std::vector<std::wstring> packageNames;
        WIN32_FIND_DATAW data{};
        HANDLE findHandle = FindFirstFileW(Combine(extractorOutput, L"*").c_str(), &data);
        if (findHandle == INVALID_HANDLE_VALUE)
        {
            return packageNames;
        }

        do
        {
            if (IsDotEntry(data.cFileName) || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                continue;
            }
            packageNames.emplace_back(data.cFileName);
        } while (FindNextFileW(findHandle, &data));

        FindClose(findHandle);
        return packageNames;
    }

    void ActivateWorkbenchWindow()
    {
        // 先精确匹配
        HWND workbench = ::FindWindowW(nullptr, L"Cx2bro v1.3.0");
        if (!workbench)
        {
            workbench = ::FindWindowW(nullptr, L"Cx2bro");
        }
        // 兜底：枚举所有窗口，部分标题匹配
        if (!workbench)
        {
            struct EnumCtx { const wchar_t* needle; HWND found; };
            EnumCtx ctx{ L"Cx2bro", nullptr };
            ::EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
                auto& ctx = *(EnumCtx*)lParam;
                wchar_t title[256];
                if (::GetWindowTextW(hwnd, title, 256) > 0 && ::wcsstr(title, ctx.needle))
                {
                    ctx.found = hwnd;
                    return FALSE;
                }
                return TRUE;
            }, (LPARAM)&ctx);
            workbench = ctx.found;
        }
        if (workbench)
        {
            ::ShowWindow(workbench, SW_RESTORE);
            ::SetForegroundWindow(workbench);
        }
    }

    const wchar_t* ToStateText(RestoreUiTaskState state)
    {
        switch (state)
        {
            case RestoreUiTaskState::Queued: return L"排队中";
            case RestoreUiTaskState::Restoring: return L"处理中";
            case RestoreUiTaskState::Completed: return L"已完成";
            case RestoreUiTaskState::Failed: return L"失败";
        }
        return L"未知";
    }

    std::wstring ToProgressText(unsigned int current, unsigned int total)
    {
        if (total == 0u)
        {
            return L"等待中";
        }
        unsigned int percent = current >= total ? 100u : (current * 100u) / total;
        wchar_t buffer[64]{};
        wsprintfW(buffer, L"%u%% (%u/%u)", percent, current, total);
        return buffer;
    }

    std::wstring ToSummaryDetail(const std::wstring& detail)
    {
        if (detail.empty())
        {
            return L"";
        }
        constexpr size_t maxChars = 72u;
        if (detail.size() <= maxChars)
        {
            return detail;
        }
        return detail.substr(0u, maxChars - 3u) + L"...";
    }

    void ScrollLogToPercent(UiContext* context, unsigned int percent)
    {
        if (!context || !context->logEdit)
        {
            return;
        }
        int lineCount = static_cast<int>(SendMessageW(context->logEdit, EM_GETLINECOUNT, 0, 0));
        if (lineCount <= 1)
        {
            return;
        }
        int targetLine = static_cast<int>((lineCount - 1) * (percent > 100u ? 100u : percent) / 100u);
        int currentLine = static_cast<int>(SendMessageW(context->logEdit, EM_GETFIRSTVISIBLELINE, 0, 0));
        SendMessageW(context->logEdit, EM_LINESCROLL, 0, targetLine - currentLine);
    }

    void AppendLog(UiContext* context, const std::wstring& text, unsigned int scrollPercent)
    {
        if (!context || !context->logEdit)
        {
            return;
        }
        int length = GetWindowTextLengthW(context->logEdit);
        SendMessageW(context->logEdit, EM_SETSEL, length, length);
        std::wstring line = text + L"\r\n";
        SendMessageW(context->logEdit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
        ScrollLogToPercent(context, scrollPercent);
    }

    unsigned int UpdateOverallSummary(UiContext* context)
    {
        if (!context)
        {
            return 0u;
        }

        unsigned int totalTasks = static_cast<unsigned int>(context->stateByPackage.size());
        unsigned int queued = 0u;
        unsigned int running = 0u;
        unsigned int done = 0u;
        unsigned int failed = 0u;
        unsigned int progressCurrent = 0u;
        unsigned int progressTotal = 0u;

        for (const auto& item : context->stateByPackage)
        {
            const RowState& state = item.second;
            if (state.state == RestoreUiTaskState::Queued) ++queued;
            else if (state.state == RestoreUiTaskState::Restoring) ++running;
            else if (state.state == RestoreUiTaskState::Completed) ++done;
            else if (state.state == RestoreUiTaskState::Failed) ++failed;

            if (state.total > 0u)
            {
                progressCurrent += state.current > state.total ? state.total : state.current;
                progressTotal += state.total;
            }
        }

        unsigned int percent = progressTotal > 0u ? (progressCurrent * 100u) / progressTotal : 0u;
        SendMessageW(context->progressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        SendMessageW(context->progressBar, PBM_SETPOS, percent, 0);

        wchar_t summary[256]{};
        wsprintfW(summary, L"总任务 %u | 运行中 %u | 排队 %u | 完成 %u | 失败 %u",
                  totalTasks, running, queued, done, failed);
        SetWindowTextW(context->statusText, summary);

        wchar_t title[256]{};
        wsprintfW(title, L"Cx2bro 资源名还原 - 运行中 %u / 排队 %u / 完成 %u", running, queued, done);
        SetWindowTextW(context->window, title);
        return percent;
    }

    void EnsureRow(UiContext* context, const std::wstring& packageName)
    {
        if (context->rowByPackage.find(packageName) != context->rowByPackage.end())
        {
            return;
        }

        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = ListView_GetItemCount(context->taskList);
        item.pszText = const_cast<LPWSTR>(packageName.c_str());
        int row = ListView_InsertItem(context->taskList, &item);
        context->rowByPackage[packageName] = row;
    }

    void UpdateRow(UiContext* context, const RestoreProgressInfo& info)
    {
        EnsureRow(context, info.packageName);
        int row = context->rowByPackage[info.packageName];
        context->stateByPackage[info.packageName] = RowState{ info.state, info.current, info.total, info.detail };

        ListView_SetItemText(context->taskList, row, 0, const_cast<LPWSTR>(info.packageName.c_str()));
        ListView_SetItemText(context->taskList, row, 1, const_cast<LPWSTR>(ToStateText(info.state)));
        std::wstring progress = ToProgressText(info.current, info.total);
        ListView_SetItemText(context->taskList, row, 2, progress.data());
        std::wstring summary = ToSummaryDetail(info.detail);
        ListView_SetItemText(context->taskList, row, 3, summary.data());
        ListView_EnsureVisible(context->taskList, row, FALSE);
        unsigned int percent = info.total > 0u ? ((info.current >= info.total ? info.total : info.current) * 100u) / info.total : 0u;
        bool shouldLog = info.state != RestoreUiTaskState::Restoring
                      || info.current == 0u
                      || info.current >= info.total
                      || context->loggedPercentByPackage[info.packageName] != percent;
        if (shouldLog)
        {
            context->loggedPercentByPackage[info.packageName] = percent;
            unsigned int overallPercent = UpdateOverallSummary(context);
            AppendLog(context, info.packageName + L" | " + ToStateText(info.state) + L" | " + info.detail, overallPercent);
            return;
        }
        UpdateOverallSummary(context);
    }

    std::wstring FormatRestoreRate(unsigned int restored, unsigned int total)
    {
        if (total == 0u)
        {
            return L"0.00%";
        }
        wchar_t buffer[32]{};
        swprintf_s(buffer, L"%.2f%%", (double)restored * 100.0 / (double)total);
        return buffer;
    }

    void SeedQueuedRows(UiContext* context)
    {
        if (!context)
        {
            return;
        }

        std::vector<std::wstring> packageNames = CollectPackageNames(Combine(context->workspace, L"Extractor_Output"));
        for (const std::wstring& packageName : packageNames)
        {
            RestoreProgressInfo info{};
            info.packageName = packageName;
            info.state = RestoreUiTaskState::Queued;
            info.current = 0u;
            info.total = 0u;
            info.detail = L"等待开始";
            UpdateRow(context, info);
        }
    }

    void OpenPath(const std::wstring& path)
    {
        ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void RestoreProgressThunk(const RestoreProgressInfo& info, void* context)
    {
        UiContext* ui = static_cast<UiContext*>(context);
        if (!ui || ui->closing || !ui->window)
        {
            return;
        }
        ProgressMessage* message = new ProgressMessage{};
        message->info = info;
        if (!PostMessageW(ui->window, WM_APP_RESTORE_PROGRESS, 0, reinterpret_cast<LPARAM>(message)))
        {
            delete message;
        }
    }

    // Inference log callback: forwards messages to UI log
    DWORD WINAPI RestoreWorkerThread(LPVOID parameter)
    {
        UiContext* context = static_cast<UiContext*>(parameter);
        FinishMessage* finish = new FinishMessage{};
        ResourceRestorerLite restorer;
        finish->success = restorer.RestoreWorkspaceEx(
            context->workspace,
            context->workerCount,
            context->enableFallback,
            finish->result,
            finish->errorMessage,
            RestoreProgressThunk,
            context);

        // Secondary inference: analyze restored filenames and recover more hash-named files
        if (finish->success && context->enableInference)
        {
            int inferred = ResourceRestorerLite::SecondaryInference(
                context->workspace,
                context->workerCount);
            finish->result.inferenceRestoredFiles = static_cast<unsigned int>(inferred);
        }

        if (!context->closing && context->window)
        {
            if (!PostMessageW(context->window, WM_APP_RESTORE_DONE, 0, reinterpret_cast<LPARAM>(finish)))
            {
                delete finish;
            }
        }
        else
        {
            delete finish;
        }
        return 0u;
    }

    void InitializeListViewColumns(HWND listView)
    {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

        column.pszText = const_cast<LPWSTR>(L"封包");
        column.cx = 86;
        ListView_InsertColumn(listView, 0, &column);

        column.pszText = const_cast<LPWSTR>(L"状态");
        column.cx = 56;
        column.iSubItem = 1;
        ListView_InsertColumn(listView, 1, &column);

        column.pszText = const_cast<LPWSTR>(L"进度");
        column.cx = 84;
        column.iSubItem = 2;
        ListView_InsertColumn(listView, 2, &column);

        column.pszText = const_cast<LPWSTR>(L"当前条目/详细信息");
        column.cx = 176;
        column.iSubItem = 3;
        ListView_InsertColumn(listView, 3, &column);

        ListView_SetExtendedListViewStyle(listView, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    }

    INT_PTR CALLBACK RestoreDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        UiContext* context = reinterpret_cast<UiContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        switch (msg)
        {
            case WM_INITDIALOG:
            {
                context = reinterpret_cast<UiContext*>(lParam);
                context->window = hwnd;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));

                SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON)));
                SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON)));

                context->taskList = GetDlgItem(hwnd, IDC_RESTORE_TASKLIST);
                context->progressBar = GetDlgItem(hwnd, IDC_RESTORE_PROGRESS);
                context->statusText = GetDlgItem(hwnd, IDC_RESTORE_STATUS);
                context->outputEdit = GetDlgItem(hwnd, IDC_RESTORE_OUTPUT);
                context->workerInfo = GetDlgItem(hwnd, IDC_RESTORE_WORKERINFO);
                context->logEdit = GetDlgItem(hwnd, IDC_RESTORE_LOG);

                SetWindowTextW(context->outputEdit, context->restoredOutput.c_str());
                SetWindowTextW(context->workerInfo, (L"工作线程: " + std::to_wstring(context->workerCount)).c_str());
                CheckDlgButton(hwnd, IDC_RESTORE_FALLBACK, context->enableFallback ? BST_CHECKED : BST_UNCHECKED);
                CheckDlgButton(hwnd, IDC_RESTORE_INFERENCE, context->enableInference ? BST_CHECKED : BST_UNCHECKED);
                InitializeListViewColumns(context->taskList);
                SeedQueuedRows(context);
                UpdateOverallSummary(context);
                AppendLog(context, L"已准备资源名还原任务。", 0u);
                AppendLog(context, L"请根据需要勾选选项，然后点击「开始」按钮。", 0u);

                EnableWindow(GetDlgItem(hwnd, IDC_RESTORE_START), TRUE);
                context->startTime = std::chrono::steady_clock::now();
                return TRUE;
            }
            case WM_COMMAND:
                switch (LOWORD(wParam))
                {
                    case IDC_RESTORE_OPEN:
                        OpenPath(context->restoredOutput);
                        return TRUE;
                    case IDC_RESTORE_FALLBACK:
                        if (context)
                            context->enableFallback = (HIWORD(wParam) == BN_CLICKED) ?
                                (SendMessageW(GetDlgItem(hwnd, IDC_RESTORE_FALLBACK), BM_GETCHECK, 0, 0) == BST_CHECKED) :
                                context->enableFallback;
                        return TRUE;
                    case IDC_RESTORE_INFERENCE:
                        if (context)
                            context->enableInference = (HIWORD(wParam) == BN_CLICKED) ?
                                (SendMessageW(GetDlgItem(hwnd, IDC_RESTORE_INFERENCE), BM_GETCHECK, 0, 0) == BST_CHECKED) :
                                context->enableInference;
                        return TRUE;
                    case IDC_RESTORE_START:
                        if (context && context->workerThread == nullptr)
                        {
                            EnableWindow(GetDlgItem(hwnd, IDC_RESTORE_START), FALSE);
                            EnableWindow(GetDlgItem(hwnd, IDC_RESTORE_FALLBACK), FALSE);
                            EnableWindow(GetDlgItem(hwnd, IDC_RESTORE_INFERENCE), FALSE);
                            AppendLog(context, L"开始还原...", 0u);
                            context->workerThread = CreateThread(nullptr, 0u, RestoreWorkerThread, context, 0u, nullptr);
                        }
                        return TRUE;
                    case IDC_RESTORE_BACK:
                        if (context)
                        {
                            context->closing = true;
                            context->window = nullptr;
                        }
                        ActivateWorkbenchWindow();
                        DestroyWindow(hwnd);
                        return TRUE;
                }
                break;
            case WM_APP_RESTORE_PROGRESS:
            {
                ProgressMessage* message = reinterpret_cast<ProgressMessage*>(lParam);
                if (message)
                {
                    UpdateRow(context, message->info);
                    delete message;
                }
                return TRUE;
            }
            case WM_APP_RESTORE_DONE:
            {
                FinishMessage* finish = reinterpret_cast<FinishMessage*>(lParam);
                if (finish)
                {
                    if (!finish->success)
                    {
                        AppendLog(context, L"还原失败: " + finish->errorMessage, 100u);
                        MessageBoxW(hwnd,
                                    finish->errorMessage.empty() ? L"资源名还原失败。" : finish->errorMessage.c_str(),
                                    L"Cx2bro 资源名还原",
                                    MB_OK | MB_ICONWARNING);
                    }
                    else
                    {
                        AppendLog(context, L"还原完成。", 100u);
                        unsigned int inferenceCount = finish->result.inferenceRestoredFiles;
                        unsigned int totalRestored = finish->result.restoredFiles + inferenceCount;
                        std::wstring primaryRate = FormatRestoreRate(finish->result.restoredFiles, finish->result.totalFiles);
                        std::wstring finalRate = FormatRestoreRate(totalRestored, finish->result.totalFiles);
                        SetWindowTextW(context->statusText,
                                       (L"还原完成 | 成功率 " + finalRate + L" | " +
                                        std::to_wstring(totalRestored) + L" / " +
                                        std::to_wstring(finish->result.totalFiles)).c_str());
                        SetWindowTextW(context->window, (L"Cx2bro 资源名还原 - 已完成 | 成功率 " + finalRate).c_str());
                        std::wstring detail = L"还原完成。\r\n\r\n";
                        detail += L"━━━ 主还原 ━━━\r\n";
                        detail += L"总文件数: " + std::to_wstring(finish->result.totalFiles) + L"\r\n";
                        detail += L"成功还原: " + std::to_wstring(finish->result.restoredFiles) + L"  (" + primaryRate + L")\r\n";
                        detail += L"缺少目录 Hash: " + std::to_wstring(finish->result.missingDirectoryHash) + L"\r\n";
                        detail += L"缺少文件名 Hash: " + std::to_wstring(finish->result.missingFileNameHash) + L"\r\n";
                        detail += L"复制失败: " + std::to_wstring(finish->result.copyFailed) + L"\r\n";
                        if (finish->result.fallbackRestoredFiles > 0)
                            detail += L"按后缀名保留（未匹配到文件名）: " + std::to_wstring(finish->result.fallbackRestoredFiles) + L"\r\n";
                        if (inferenceCount > 0)
                        {
                            detail += L"\r\n━━━ 推理补充还原 ━━━\r\n";
                            // Read report file for per-directory breakdown
                            std::wstring reportPath = Combine(context->restoredOutput, L"RestoreReport.txt");
                            std::wstring reportContent;
                            if (ReadUtf16File(reportPath, reportContent))
                            {
                                // Extract inference section
                                size_t infSec = reportContent.find(L"--- 推理补充还原 ---");
                                if (infSec != std::wstring::npos)
                                {
                                    size_t endSec = reportContent.find(L"\r\n\r\n", infSec);
                                    if (endSec == std::wstring::npos) endSec = reportContent.size();
                                    std::wstring infText = reportContent.substr(infSec, endSec - infSec);
                                    // Remove "--- 推理补充还原 ---" header
                                    size_t nl = infText.find(L"\r\n");
                                    if (nl != std::wstring::npos) infText = infText.substr(nl + 2);
                                    detail += infText;
                                }
                                // Also extract updated per-directory section
                                size_t dirSec = reportContent.find(L"--- 各目录还原情况 ---");
                                if (dirSec != std::wstring::npos)
                                {
                                    detail += L"\r\n━━━ 各目录还原情况 ━━━\r\n";
                                    size_t secStart = dirSec;
                                    size_t headerEnd = reportContent.find(L"\r\n", secStart) + 2;
                                    size_t secEnd = reportContent.find(L"\r\n\r\n---", headerEnd);
                                    if (secEnd == std::wstring::npos) secEnd = reportContent.find(L"\r\n\r\n", headerEnd);
                                    if (secEnd != std::wstring::npos && secEnd > headerEnd)
                                    {
                                        detail += reportContent.substr(headerEnd, secEnd - headerEnd);
                                    }
                                }
                            }
                            else
                            {
                                detail += L"推理补充: +" + std::to_wstring(inferenceCount) + L"\r\n";
                            }
                        }
                        else
                        {
                            // No inference - show per-directory from result struct
                            detail += L"\r\n━━━ 各目录还原情况 ━━━\r\n";
                            for (const auto& pkg : finish->result.packages)
                            {
                                if (pkg.totalFiles > 0)
                                {
                                    std::wstring name = pkg.packageName;
                                    std::wstring rate = FormatRestoreRate(pkg.restoredFiles, pkg.totalFiles);
                                    detail += name + L"│" + std::to_wstring(pkg.restoredFiles)
                                           + L"/" + std::to_wstring(pkg.totalFiles)
                                           + L"│" + rate + L"\r\n";
                                }
                            }
                        }
                        detail += L"\r\n━━━ 最终结果 ━━━\r\n";
                        detail += L"最终还原: " + std::to_wstring(totalRestored) + L" / " + std::to_wstring(finish->result.totalFiles)
                                  + L"  (成功率 " + finalRate + L")\r\n";
                        detail += L"剩余未还原: " + std::to_wstring(totalRestored > finish->result.totalFiles ? 0 : finish->result.totalFiles - totalRestored) + L"\r\n\r\n";
                        // 还原耗时
                        auto elapsed = std::chrono::steady_clock::now() - context->startTime;
                        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
                        detail += L"\r\n还原耗时: ";
                        if (ms >= 60000)
                            detail += std::to_wstring(ms / 60000) + L"分 " + std::to_wstring((ms % 60000) / 1000) + L"秒";
                        else
                            detail += std::to_wstring(ms / 1000) + L"." + std::to_wstring(ms % 1000) + L"秒";
                        detail += L"\r\n报告: " + finish->result.reportPath;
                        MessageBoxW(hwnd, detail.c_str(), L"Cx2bro 资源名还原", MB_OK | MB_ICONINFORMATION);
                    }
                    delete finish;
                }
                // 还原结束后重新启用按钮
                if (context && context->window)
                {
                    EnableWindow(GetDlgItem(context->window, IDC_RESTORE_START), TRUE);
                    EnableWindow(GetDlgItem(context->window, IDC_RESTORE_FALLBACK), TRUE);
                    EnableWindow(GetDlgItem(context->window, IDC_RESTORE_INFERENCE), TRUE);
                    if (context->workerThread)
                    {
                        CloseHandle(context->workerThread);
                        context->workerThread = nullptr;
                    }
                }
                return TRUE;
            }
            case WM_CLOSE:
                if (context)
                {
                    context->closing = true;
                    context->window = nullptr;
                }
                DestroyWindow(hwnd);
                return TRUE;
            case WM_DESTROY:
                if (context && context->workerThread)
                {
                    CloseHandle(context->workerThread);
                    context->workerThread = nullptr;
                }
                PostQuitMessage(0);
                return TRUE;
        }
        return FALSE;
    }
}

int RunRestoreUi(const std::wstring& workspace,
                 unsigned int workerCount,
                 const std::wstring& /*iconPath*/)
{
    INITCOMMONCONTROLSEX init{};
    init.dwSize = sizeof(init);
    init.dwICC = ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&init);

    UiContext context{};
    context.workspace = workspace;
    context.restoredOutput = Combine(workspace, L"Restored_Extractor_Output");
    context.workerCount = workerCount == 0u ? 1u : workerCount;

    HWND hwnd = CreateDialogParamW(GetModuleHandleW(nullptr),
                                   MAKEINTRESOURCEW(IDD_RESTORE_UI),
                                   nullptr,
                                   RestoreDialogProc,
                                   reinterpret_cast<LPARAM>(&context));
    if (!hwnd)
    {
        return 1;
    }

    ShowWindow(hwnd, SW_NORMAL);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0u, 0u) > 0)
    {
        if (!IsDialogMessageW(hwnd, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}
