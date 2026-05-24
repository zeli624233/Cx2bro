#include "PublisherTestUiLite.h"

#include "../resource.h"
#include "ResourceRestorerLite.h"
#include "StaticHashGeneratorLite.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

namespace
{
    constexpr UINT WM_APP_TEST_PROGRESS = WM_APP + 0x501u;
    constexpr UINT WM_APP_TEST_DONE = WM_APP + 0x502u;
    constexpr int IDC_TEST_BACK_WORKBENCH = 5101;

    struct TestResult
    {
        bool success = false;
        StaticHashGeneratorLite::Result hashResult{};
        ResourceRestorerLite::Result restoreResult{};
        std::wstring errorMessage;
        unsigned long long packageBytes = 0;
        unsigned int baselineRestored = 0;
        unsigned int baselineThreshold = 0;
        bool passedSize = false;
        bool passedBaseline = false;
        bool passedValidation = false;
    };

    struct ProgressMessage
    {
        RestoreProgressInfo info;
    };

    struct UiContext
    {
        std::wstring workspace;
        std::wstring extensionDirectory;
        HWND window = nullptr;
        HWND taskList = nullptr;
        HWND progressBar = nullptr;
        HWND statusText = nullptr;
        HWND outputEdit = nullptr;
        HWND workerInfo = nullptr;
        HWND logEdit = nullptr;
        HANDLE workerThread = nullptr;
        bool closing = false;
        TestResult result{};
        std::unordered_map<std::wstring, int> rowByPackage;
        std::unordered_map<std::wstring, RestoreProgressInfo> stateByPackage;
        std::unordered_map<std::wstring, unsigned int> loggedPercentByPackage;
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

    std::vector<std::wstring> CollectPackageNames(const std::wstring& extractorOutput)
    {
        std::vector<std::wstring> packageNames;
        WIN32_FIND_DATAW data{};
        HANDLE findHandle = FindFirstFileW(Combine(extractorOutput, L"*").c_str(), &data);
        if (findHandle == INVALID_HANDLE_VALUE) return packageNames;
        do
        {
            if (!IsDotEntry(data.cFileName) && (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                packageNames.emplace_back(data.cFileName);
            }
        } while (FindNextFileW(findHandle, &data));
        FindClose(findHandle);
        return packageNames;
    }

    std::string WideToUtf8(const std::wstring& value)
    {
        if (value.empty()) return "";
        int length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string output(length ? length - 1 : 0, '\0');
        if (length > 1) WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, output.data(), length, nullptr, nullptr);
        return output;
    }

    void PrintLine(const std::wstring& text)
    {
        std::string output = WideToUtf8(text + L"\n");
        ::WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output.c_str(), (DWORD)output.size(), nullptr, nullptr);
    }

    std::wstring FormatRate(unsigned int restored, unsigned int total)
    {
        if (total == 0u) return L"0.00%";
        wchar_t buffer[32]{};
        swprintf_s(buffer, L"%.2f%%", (double)restored * 100.0 / (double)total);
        return buffer;
    }

    std::wstring FormatKb(unsigned long long bytes)
    {
        wchar_t buffer[32]{};
        swprintf_s(buffer, L"%.1f", (double)bytes / 1024.0);
        return buffer;
    }

    unsigned long long FileSizeOf(const std::wstring& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        {
            return 0;
        }
        ULARGE_INTEGER value{};
        value.HighPart = data.nFileSizeHigh;
        value.LowPart = data.nFileSizeLow;
        return value.QuadPart;
    }

    unsigned long long DirectoryFileSize(const std::wstring& directory)
    {
        unsigned long long total = 0;
        WIN32_FIND_DATAW data{};
        HANDLE findHandle = FindFirstFileW(Combine(directory, L"*").c_str(), &data);
        if (findHandle == INVALID_HANDLE_VALUE) return 0;
        do
        {
            if (IsDotEntry(data.cFileName)) continue;
            std::wstring path = Combine(directory, data.cFileName);
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                total += DirectoryFileSize(path);
            }
            else
            {
                ULARGE_INTEGER value{};
                value.HighPart = data.nFileSizeHigh;
                value.LowPart = data.nFileSizeLow;
                total += value.QuadPart;
            }
        } while (FindNextFileW(findHandle, &data));
        FindClose(findHandle);
        return total;
    }

    unsigned long long PackageSizeBytes(const std::wstring& extensionDirectory)
    {
        return FileSizeOf(Combine(extensionDirectory, L"manifest.int"))
             + FileSizeOf(Combine(extensionDirectory, L"rules.int"));
    }

    bool WriteUtf8File(const std::wstring& path, const std::wstring& content)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
        WriteFile(file, bom, sizeof(bom), &written, nullptr);
        std::string output = WideToUtf8(content);
        BOOL ok = WriteFile(file, output.data(), static_cast<DWORD>(output.size()), &written, nullptr);
        CloseHandle(file);
        return ok == TRUE;
    }

    std::wstring Utf8ToWide(const std::string& value)
    {
        if (value.empty()) return L"";
        const unsigned char* bytes = reinterpret_cast<const unsigned char*>(value.data());
        int offset = value.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF ? 3 : 0;
        int length = MultiByteToWideChar(CP_UTF8, 0, value.data() + offset, (int)value.size() - offset, nullptr, 0);
        std::wstring output(length, L'\0');
        if (length > 0) MultiByteToWideChar(CP_UTF8, 0, value.data() + offset, (int)value.size() - offset, output.data(), length);
        return output;
    }

    std::wstring ReadUtf8File(const std::wstring& path)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return L"";
        DWORD size = GetFileSize(file, nullptr);
        std::string buffer(size, '\0');
        DWORD read = 0;
        if (size > 0) ReadFile(file, buffer.data(), size, &read, nullptr);
        CloseHandle(file);
        buffer.resize(read);
        return Utf8ToWide(buffer);
    }

    std::wstring ReadUtf16File(const std::wstring& path)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return L"";
        DWORD size = GetFileSize(file, nullptr);
        std::wstring output(size / sizeof(wchar_t), L'\0');
        DWORD read = 0;
        if (size > 0) ReadFile(file, output.data(), size, &read, nullptr);
        CloseHandle(file);
        output.resize(read / sizeof(wchar_t));
        if (!output.empty() && output.front() == 0xFEFF) output.erase(output.begin());
        return output;
    }

    unsigned int ParseReportNumber(const std::wstring& content, const std::wstring& key)
    {
        size_t pos = content.find(key);
        if (pos == std::wstring::npos) return 0;
        pos += key.size();
        while (pos < content.size() && (content[pos] == L':' || content[pos] == L' ' || content[pos] == L'\t')) ++pos;
        unsigned int value = 0;
        while (pos < content.size() && content[pos] >= L'0' && content[pos] <= L'9')
        {
            value = value * 10u + static_cast<unsigned int>(content[pos] - L'0');
            ++pos;
        }
        return value;
    }

    unsigned int ReadBaselineRestored(const std::wstring& workspace)
    {
        std::wstring report = ReadUtf16File(Combine(Combine(workspace, L"Restored_Extractor_Output"), L"RestoreReport.txt"));
        if (report.empty()) return 0;
        return ParseReportNumber(report, L"成功还原");
    }

    std::wstring StripValidationSection(const std::wstring& content)
    {
        std::wstring output;
        size_t position = 0;
        while (position < content.size())
        {
            size_t lineEnd = content.find(L'\n', position);
            size_t next = lineEnd == std::wstring::npos ? content.size() : lineEnd + 1;
            std::wstring line = content.substr(position, next - position);
            std::wstring trimmed = line;
            while (!trimmed.empty() && (trimmed.back() == L'\r' || trimmed.back() == L'\n' || trimmed.back() == L' ' || trimmed.back() == L'\t')) trimmed.pop_back();
            if (_wcsicmp(trimmed.c_str(), L"[Validation]") == 0)
            {
                position = next;
                while (position < content.size())
                {
                    size_t innerEnd = content.find(L'\n', position);
                    size_t innerNext = innerEnd == std::wstring::npos ? content.size() : innerEnd + 1;
                    std::wstring innerLine = content.substr(position, innerNext - position);
                    std::wstring innerTrimmed = innerLine;
                    while (!innerTrimmed.empty() && (innerTrimmed.back() == L'\r' || innerTrimmed.back() == L'\n' || innerTrimmed.back() == L' ' || innerTrimmed.back() == L'\t')) innerTrimmed.pop_back();
                    if (!innerTrimmed.empty() && innerTrimmed.front() == L'[')
                    {
                        break;
                    }
                    position = innerNext;
                }
                continue;
            }
            output += line;
            position = next;
        }
        while (!output.empty() && (output.back() == L'\r' || output.back() == L'\n')) output.pop_back();
        return output;
    }

    void WriteValidationToIni(const std::wstring& path, const TestResult& result)
    {
        std::wstring content = StripValidationSection(ReadUtf8File(path));
        std::wstring validation = L"\r\n\r\n[Validation]\r\n";
        validation += L"Passed = ";
        validation += result.passedValidation ? L"yes\r\n" : L"no\r\n";
        validation += L"TotalFiles = " + std::to_wstring(result.success ? result.restoreResult.totalFiles : 0) + L"\r\n";
        validation += L"RestoredFiles = " + std::to_wstring(result.success ? result.restoreResult.restoredFiles : 0) + L"\r\n";
        validation += L"SuccessRate = " + (result.success ? FormatRate(result.restoreResult.restoredFiles, result.restoreResult.totalFiles) : L"-") + L"\r\n";
        validation += L"PackageBytes = " + std::to_wstring(result.packageBytes) + L"\r\n";
        validation += L"PackageSizeKB = " + FormatKb(result.packageBytes) + L"\r\n";
        validation += L"PackageLimitBytes = 40960\r\n";
        validation += L"BaselineRestoredFiles = " + std::to_wstring(result.baselineRestored) + L"\r\n";
        validation += L"BaselineThresholdFiles = " + std::to_wstring(result.baselineThreshold) + L"\r\n";
        validation += L"BaselinePassRatio = 0.75\r\n";
        validation += L"PassBasis = 包体 " + FormatKb(result.packageBytes) + L" KB / 上限 40 KB：";
        validation += result.passedSize ? L"通过" : L"未通过";
        validation += L"；成功还原 " + std::to_wstring(result.success ? result.restoreResult.restoredFiles : 0)
                   + L" / 门槛 " + std::to_wstring(result.baselineThreshold)
                   + L"（基线 " + std::to_wstring(result.baselineRestored) + L" 的 75%）：";
        validation += result.passedBaseline ? L"通过。\r\n" : L"未通过。\r\n";
        validation += L"Error = ";
        if (!result.passedValidation)
        {
            validation += result.errorMessage.empty() ? L"测试结果低于基线 75% 或包体超过限制。" : result.errorMessage;
        }
        validation += L"\r\n";
        WriteUtf8File(path, content + validation);
    }

    void ActivateWorkbenchWindow()
    {
        // 先精确匹配
        HWND workbench = ::FindWindowW(nullptr, L"Cx2bro v1.0.0");
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

    std::wstring ProgressText(unsigned int current, unsigned int total)
    {
        if (total == 0u) return L"等待中";
        unsigned int percent = current >= total ? 100u : (current * 100u) / total;
        wchar_t buffer[64]{};
        wsprintfW(buffer, L"%u%% (%u/%u)", percent, current, total);
        return buffer;
    }

    void ScrollLogToPercent(UiContext* context, unsigned int percent)
    {
        if (!context || !context->logEdit) return;
        int lineCount = static_cast<int>(SendMessageW(context->logEdit, EM_GETLINECOUNT, 0, 0));
        if (lineCount <= 1) return;
        int targetLine = static_cast<int>((lineCount - 1) * (percent > 100u ? 100u : percent) / 100u);
        int currentLine = static_cast<int>(SendMessageW(context->logEdit, EM_GETFIRSTVISIBLELINE, 0, 0));
        SendMessageW(context->logEdit, EM_LINESCROLL, 0, targetLine - currentLine);
    }

    void AppendLog(UiContext* context, const std::wstring& text, unsigned int scrollPercent)
    {
        if (!context || !context->logEdit) return;
        int length = GetWindowTextLengthW(context->logEdit);
        SendMessageW(context->logEdit, EM_SETSEL, length, length);
        std::wstring line = text + L"\r\n";
        SendMessageW(context->logEdit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
        ScrollLogToPercent(context, scrollPercent);
    }

    void EnsureRow(UiContext* context, const std::wstring& packageName)
    {
        if (context->rowByPackage.find(packageName) != context->rowByPackage.end()) return;
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = ListView_GetItemCount(context->taskList);
        item.pszText = const_cast<LPWSTR>(packageName.c_str());
        int row = ListView_InsertItem(context->taskList, &item);
        context->rowByPackage[packageName] = row;
    }

    void SetRow(UiContext* context,
                const std::wstring& name,
                RestoreUiTaskState state,
                unsigned int current,
                unsigned int total,
                const std::wstring& detail)
    {
        EnsureRow(context, name);
        int row = context->rowByPackage[name];
        RestoreProgressInfo info{};
        info.packageName = name;
        info.state = state;
        info.current = current;
        info.total = total;
        info.detail = detail;
        context->stateByPackage[name] = info;

        ListView_SetItemText(context->taskList, row, 0, const_cast<LPWSTR>(name.c_str()));
        ListView_SetItemText(context->taskList, row, 1, const_cast<LPWSTR>(ToStateText(state)));
        std::wstring progress = ProgressText(current, total);
        ListView_SetItemText(context->taskList, row, 2, progress.data());
        ListView_SetItemText(context->taskList, row, 3, const_cast<LPWSTR>(detail.c_str()));
        ListView_EnsureVisible(context->taskList, row, FALSE);
    }

    void SetProgress(UiContext* context, int value, const std::wstring& status)
    {
        SendMessageW(context->progressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        SendMessageW(context->progressBar, PBM_SETPOS, value, 0);
        SetWindowTextW(context->statusText, status.c_str());
        SetWindowTextW(context->window, (L"Cx2bro 扩展集测试 - " + status).c_str());
    }

    int RestorePercent(UiContext* context)
    {
        unsigned int current = 0u;
        unsigned int total = 0u;
        for (const auto& item : context->stateByPackage)
        {
            if (item.first == L"生成静态 Hash" || item.first == L"记录测试结果") continue;
            const RestoreProgressInfo& info = item.second;
            if (info.total == 0u) continue;
            current += info.current > info.total ? info.total : info.current;
            total += info.total;
        }
        if (total == 0u) return 30;
        return 30 + (int)(current * 65u / total);
    }

    void RestoreProgressBridge(const RestoreProgressInfo& info, void* raw)
    {
        UiContext* context = reinterpret_cast<UiContext*>(raw);
        if (!context || context->closing || !context->window) return;
        ProgressMessage* message = new ProgressMessage{};
        message->info = info;
        if (!PostMessageW(context->window, WM_APP_TEST_PROGRESS, 0, reinterpret_cast<LPARAM>(message)))
        {
            delete message;
        }
    }

    DWORD WINAPI WorkerProc(void* raw)
    {
        UiContext* context = reinterpret_cast<UiContext*>(raw);
        TestResult result{};
        std::wstring error;
        result.packageBytes = PackageSizeBytes(context->extensionDirectory);
        result.baselineRestored = ReadBaselineRestored(context->workspace);
        result.baselineThreshold = result.baselineRestored == 0 ? 0 : (result.baselineRestored * 75u + 99u) / 100u;

        StaticHashGeneratorLite generator;
        if (!generator.GenerateFromExtension(context->extensionDirectory,
                                             Combine(context->workspace, L"StaticHash_Output"),
                                             result.hashResult,
                                             error))
        {
            result.errorMessage = error;
            context->result = result;
            PostMessageW(context->window, WM_APP_TEST_DONE, 0, 0);
            return 0;
        }
        context->result.hashResult = result.hashResult;

        ResourceRestorerLite restorer;
        if (!restorer.RestoreWorkspaceEx(context->workspace, 1, result.restoreResult, error, RestoreProgressBridge, context))
        {
            result.errorMessage = error;
            context->result = result;
            PostMessageW(context->window, WM_APP_TEST_DONE, 0, 0);
            return 0;
        }

        result.success = true;
        result.passedSize = result.packageBytes <= 40ull * 1024ull;
        result.passedBaseline = result.baselineThreshold == 0 || result.restoreResult.restoredFiles >= result.baselineThreshold;
        result.passedValidation = result.passedSize && result.passedBaseline;
        if (!result.passedValidation)
        {
            result.errorMessage = L"测试结果低于基线 75% 或包体超过限制。";
        }
        context->result = result;
        PostMessageW(context->window, WM_APP_TEST_DONE, 0, 0);
        return 0;
    }

    void InitializeColumns(HWND listView)
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

    void PrintResult(const TestResult& result)
    {
        if (!result.hashResult.outputDirectory.empty())
        {
            PrintLine(L"STATIC_HASH: generated");
            PrintLine(L"STATIC_HASH_OUTPUT: " + result.hashResult.outputDirectory);
            PrintLine(L"RESOURCE_PATHS: " + std::to_wstring(result.hashResult.resourcePathCount));
            PrintLine(L"DIRECTORY_HASHES: " + std::to_wstring(result.hashResult.directoryHashCount));
            PrintLine(L"FILE_NAME_HASHES: " + std::to_wstring(result.hashResult.fileNameHashCount));
        }
        if (result.success)
        {
            PrintLine(L"RESTORE: completed");
            PrintLine(L"RESTORE_REPORT: " + result.restoreResult.reportPath);
            PrintLine(L"TOTAL_FILES: " + std::to_wstring(result.restoreResult.totalFiles));
            PrintLine(L"RESTORED_FILES: " + std::to_wstring(result.restoreResult.restoredFiles));
            PrintLine(L"MISSING_DIRECTORY_HASH: " + std::to_wstring(result.restoreResult.missingDirectoryHash));
            PrintLine(L"MISSING_FILE_NAME_HASH: " + std::to_wstring(result.restoreResult.missingFileNameHash));
            PrintLine(L"COPY_FAILED: " + std::to_wstring(result.restoreResult.copyFailed));
        }
        else
        {
            PrintLine(L"RESTORE: failed");
            PrintLine(L"RESTORE_ERROR: " + result.errorMessage);
        }
    }

    void WriteMachineResult(UiContext* context)
    {
        if (!context) return;
        const TestResult& result = context->result;
        std::wstring text;
        if (result.success)
        {
            text += L"RESTORE=completed\r\n";
            text += L"PASSED=" + std::wstring(result.passedValidation ? L"yes" : L"no") + L"\r\n";
            text += L"TOTAL_FILES=" + std::to_wstring(result.restoreResult.totalFiles) + L"\r\n";
            text += L"RESTORED_FILES=" + std::to_wstring(result.restoreResult.restoredFiles) + L"\r\n";
            text += L"SUCCESS_RATE=" + FormatRate(result.restoreResult.restoredFiles, result.restoreResult.totalFiles) + L"\r\n";
            text += L"PACKAGE_BYTES=" + std::to_wstring(result.packageBytes) + L"\r\n";
            text += L"PACKAGE_SIZE_KB=" + FormatKb(result.packageBytes) + L"\r\n";
            text += L"PACKAGE_LIMIT_BYTES=40960\r\n";
            text += L"BASELINE_RESTORED_FILES=" + std::to_wstring(result.baselineRestored) + L"\r\n";
            text += L"BASELINE_THRESHOLD_FILES=" + std::to_wstring(result.baselineThreshold) + L"\r\n";
            text += L"RESTORE_REPORT=" + result.restoreResult.reportPath + L"\r\n";
        }
        else
        {
            text += L"RESTORE=failed\r\n";
            text += L"RESTORE_ERROR=" + result.errorMessage + L"\r\n";
        }
        WriteUtf8File(Combine(context->extensionDirectory, L"PublisherTestResult.ini"), text);
        WriteValidationToIni(Combine(context->extensionDirectory, L"manifest.int"), result);
    }

    INT_PTR CALLBACK TestDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
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
                SetWindowTextW(hwnd, L"Cx2bro 扩展集测试");

                context->taskList = GetDlgItem(hwnd, IDC_RESTORE_TASKLIST);
                context->progressBar = GetDlgItem(hwnd, IDC_RESTORE_PROGRESS);
                context->statusText = GetDlgItem(hwnd, IDC_RESTORE_STATUS);
                context->outputEdit = GetDlgItem(hwnd, IDC_RESTORE_OUTPUT);
                context->workerInfo = GetDlgItem(hwnd, IDC_RESTORE_WORKERINFO);
                context->logEdit = GetDlgItem(hwnd, IDC_RESTORE_LOG);

                SetWindowTextW(GetDlgItem(hwnd, IDC_RESTORE_HINT), L"测试扩展集");
                SetWindowTextW(context->outputEdit, context->extensionDirectory.c_str());
                SetWindowTextW(context->workerInfo, L"工作线程: 1");
                SetWindowTextW(GetDlgItem(hwnd, IDC_RESTORE_OPEN), L"打开扩展集目录");
                SetWindowTextW(GetDlgItem(hwnd, IDC_RESTORE_BACK), L"关闭");
                RECT openRect{};
                if (GetWindowRect(GetDlgItem(hwnd, IDC_RESTORE_OPEN), &openRect))
                {
                    POINT leftTop{ openRect.left, openRect.top };
                    POINT rightBottom{ openRect.right, openRect.bottom };
                    ScreenToClient(hwnd, &leftTop);
                    ScreenToClient(hwnd, &rightBottom);
                    int width = rightBottom.x - leftTop.x;
                    int height = rightBottom.y - leftTop.y;
                    HWND backButton = CreateWindowW(L"BUTTON", L"返回主程序",
                                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                                    leftTop.x - width - 8, leftTop.y, width, height,
                                                    hwnd,
                                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TEST_BACK_WORKBENCH)),
                                                    GetModuleHandleW(nullptr),
                                                    nullptr);
                    SendMessageW(backButton, WM_SETFONT, SendMessageW(GetDlgItem(hwnd, IDC_RESTORE_OPEN), WM_GETFONT, 0, 0), TRUE);
                }
                InitializeColumns(context->taskList);
                SetRow(context, L"生成静态 Hash", RestoreUiTaskState::Restoring, 0, 1, L"根据当前扩展集生成 StaticHash_Output");
                for (const std::wstring& packageName : CollectPackageNames(Combine(context->workspace, L"Extractor_Output")))
                {
                    SetRow(context, packageName, RestoreUiTaskState::Queued, 0, 0, L"等待开始");
                }
                SetProgress(context, 5, L"正在生成静态 Hash...");
                AppendLog(context, L"已准备扩展集测试任务。", 0u);
                context->workerThread = CreateThread(nullptr, 0, WorkerProc, context, 0, nullptr);
                return TRUE;
            }
            case WM_COMMAND:
                if (LOWORD(wParam) == IDC_RESTORE_OPEN && context)
                {
                    ShellExecuteW(hwnd, L"open", context->extensionDirectory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    return TRUE;
                }
                if (LOWORD(wParam) == IDC_TEST_BACK_WORKBENCH)
                {
                    ActivateWorkbenchWindow();
                    if (context)
                    {
                        context->closing = true;
                        context->window = nullptr;
                    }
                    DestroyWindow(hwnd);
                    return TRUE;
                }
                if (LOWORD(wParam) == IDC_RESTORE_BACK || LOWORD(wParam) == IDCANCEL)
                {
                    if (context)
                    {
                        context->closing = true;
                        context->window = nullptr;
                    }
                    DestroyWindow(hwnd);
                    return TRUE;
                }
                break;
            case WM_APP_TEST_PROGRESS:
            {
                ProgressMessage* message = reinterpret_cast<ProgressMessage*>(lParam);
                if (message)
                {
                    const RestoreProgressInfo& info = message->info;
                    SetRow(context, L"生成静态 Hash", RestoreUiTaskState::Completed, 1, 1,
                           L"候选 " + std::to_wstring(context->result.hashResult.resourcePathCount));
                    SetRow(context, info.packageName, info.state, info.current, info.total, info.detail);
                    SetProgress(context, RestorePercent(context), L"正在还原资源名...");
                    unsigned int logPercent = info.total > 0u ? ((info.current >= info.total ? info.total : info.current) * 100u) / info.total : 0u;
                    bool shouldLog = info.state != RestoreUiTaskState::Restoring
                                  || info.current == 0u
                                  || info.current >= info.total
                                  || context->loggedPercentByPackage[info.packageName] != logPercent;
                    if (shouldLog)
                    {
                        context->loggedPercentByPackage[info.packageName] = logPercent;
                        AppendLog(context, info.packageName + L" | " + ToStateText(info.state) + L" | " + info.detail,
                                  static_cast<unsigned int>(RestorePercent(context)));
                    }
                    delete message;
                }
                return TRUE;
            }
            case WM_APP_TEST_DONE:
            {
                const TestResult& result = context->result;
                WriteMachineResult(context);
                if (result.hashResult.resourcePathCount > 0)
                {
                    SetRow(context, L"生成静态 Hash", RestoreUiTaskState::Completed, 1, 1,
                           L"候选 " + std::to_wstring(result.hashResult.resourcePathCount));
                }
                if (result.success)
                {
                    std::wstring rate = FormatRate(result.restoreResult.restoredFiles, result.restoreResult.totalFiles);
                    SetRow(context, L"记录测试结果", RestoreUiTaskState::Completed, 1, 1,
                           std::to_wstring(result.restoreResult.restoredFiles) + L" / " +
                           std::to_wstring(result.restoreResult.totalFiles) + L"，成功率 " + rate);
                    SetProgress(context, 100, L"测试完成 | 成功率 " + rate);
                    AppendLog(context, L"测试完成，成功率 " + rate, 100u);
                }
                else
                {
                    SetRow(context, L"记录测试结果", RestoreUiTaskState::Failed, 0, 1, result.errorMessage);
                    SetProgress(context, 100, L"测试失败");
                    AppendLog(context, L"测试失败: " + result.errorMessage, 100u);
                }
                MessageBeep(result.success ? MB_OK : MB_ICONWARNING);
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

int RunPublisherTestUi(const std::wstring& workspace,
                       const std::wstring& extensionDirectory,
                       const std::wstring& /*iconPath*/)
{
    INITCOMMONCONTROLSEX init{};
    init.dwSize = sizeof(init);
    init.dwICC = ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&init);

    UiContext context{};
    context.workspace = workspace;
    context.extensionDirectory = extensionDirectory;

    HWND hwnd = CreateDialogParamW(GetModuleHandleW(nullptr),
                                   MAKEINTRESOURCEW(IDD_RESTORE_UI),
                                   nullptr,
                                   TestDialogProc,
                                   reinterpret_cast<LPARAM>(&context));
    if (!hwnd) return 5;

    ShowWindow(hwnd, SW_NORMAL);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(hwnd, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (context.workerThread)
    {
        WaitForSingleObject(context.workerThread, INFINITE);
        CloseHandle(context.workerThread);
    }
    PrintResult(context.result);
    return context.result.success ? 0 : 4;
}
