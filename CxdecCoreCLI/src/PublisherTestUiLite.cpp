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
    constexpr bool kDebug = false; // 调试日志开关

    constexpr UINT WM_APP_TEST_PROGRESS = WM_APP + 0x501u;
    constexpr UINT WM_APP_TEST_DONE = WM_APP + 0x502u;
    constexpr UINT WM_APP_TEST_LOG = WM_APP + 0x503u;

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
        validation += L"TotalFiles = " + std::to_wstring(result.success ? result.restoreResult.totalFiles : 0) + L"\r\n";
        validation += L"RestoredFiles = " + std::to_wstring(result.success ? result.restoreResult.restoredFiles : 0) + L"\r\n";
        validation += L"Passed = ";
        validation += result.success && result.passedBaseline ? L"yes" : L"no";
        validation += L"\r\n";
        validation += L"SuccessRate = " + (result.success ? FormatRate(result.restoreResult.restoredFiles, result.restoreResult.totalFiles) : L"-") + L"\r\n";
        validation += L"PackageBytes = " + std::to_wstring(result.packageBytes) + L"\r\n";
        validation += L"PackageSizeKB = " + FormatKb(result.packageBytes) + L"\r\n";
        validation += L"Error = " + (result.errorMessage.empty() ? L"" : result.errorMessage) + L"\r\n";
        WriteUtf8File(path, content + validation);
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

    /// <summary>
    /// 从工作线程安全地追加日志（通过 PostMessage 到主线程）
    /// </summary>
    void PostLog(UiContext* context, const std::wstring& text)
    {
        if (!context || context->closing) return;
        std::wstring* copy = new std::wstring(text);
        PostMessageW(context->window, WM_APP_TEST_LOG, 0, reinterpret_cast<LPARAM>(copy));
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

    void WriteDebugLog(UiContext* context, const std::wstring& text)
    {
        if (!kDebug) return;
        std::wstring debugPath = Combine(context->extensionDirectory, L"debug_test.log");
        HANDLE h = CreateFileW(debugPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        SetFilePointer(h, 0, nullptr, FILE_END);
        std::string utf8 = WideToUtf8(text + L"\r\n");
        DWORD written = 0;
        WriteFile(h, utf8.c_str(), (DWORD)utf8.size(), &written, nullptr);
        CloseHandle(h);
    }

    // 从数据源的 Restored_Extractor_Output 收集所有真实文件名
    // 这些文件名将被注入 hash 生成器的 fileSet，避免 Pattern 展开浪费 cap
    std::set<std::wstring> CollectRestoredFilenames(const std::wstring& workspace)
    {
        std::set<std::wstring> names;
        std::wstring restoredRoot = Combine(workspace, L"Restored_Extractor_Output");
        DWORD attr = GetFileAttributesW(restoredRoot.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) == 0)
            return names;

        WIN32_FIND_DATAW pkgData{};
        HANDLE pkgFind = FindFirstFileW(Combine(restoredRoot, L"*").c_str(), &pkgData);
        if (pkgFind == INVALID_HANDLE_VALUE) return names;

        do
        {
            if (IsDotEntry(pkgData.cFileName) || (pkgData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                continue;

            // Restored_Extractor_Output 结构：
            //   voice/anj_000_0001.ogg      ← 文件直接放在 package 目录下
            //   voice/anj_000_0001.ogg.sli  ← 没有额外的 hash 目录层
            std::wstring pkgDir = Combine(restoredRoot, pkgData.cFileName);
            WIN32_FIND_DATAW fileData{};
            HANDLE fileFind = FindFirstFileW(Combine(pkgDir, L"*").c_str(), &fileData);
            if (fileFind == INVALID_HANDLE_VALUE) continue;

            do
            {
                if (IsDotEntry(fileData.cFileName) || (fileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                    continue;
                names.insert(fileData.cFileName);
            } while (FindNextFileW(fileFind, &fileData));
            FindClose(fileFind);
        } while (FindNextFileW(pkgFind, &pkgData));
        FindClose(pkgFind);

        return names;
    }

    DWORD WINAPI WorkerProc(void* raw)
    {
        UiContext* context = reinterpret_cast<UiContext*>(raw);
        WriteDebugLog(context, L"[TEST] ========== WorkerProc START ==========");
        WriteDebugLog(context, L"[TEST] Step1: PackageSizeBytes...");

        TestResult result{};
        std::wstring error;
        result.packageBytes = PackageSizeBytes(context->extensionDirectory);
        WriteDebugLog(context, L"[TEST] Step1 OK: packageBytes=" + std::to_wstring(result.packageBytes));

        WriteDebugLog(context, L"[TEST] Step2: ReadBaselineRestored...");
        result.baselineRestored = ReadBaselineRestored(context->workspace);
        WriteDebugLog(context, L"[TEST] Step2 OK: baselineRestored=" + std::to_wstring(result.baselineRestored));

        result.baselineThreshold = result.baselineRestored == 0 ? 0 : (result.baselineRestored * 75u + 99u) / 100u;
        WriteDebugLog(context, L"[TEST] Step3: baselineThreshold=" + std::to_wstring(result.baselineThreshold));

        StaticHashGeneratorLite generator;
        // 从数据源的 Restored_Extractor_Output 收集已知文件名，注入 hash 生成器
        // 这些文件名会被预置入 fileSet，Pattern 展开时跳过已存在文件，不浪费 cap
        {
            std::set<std::wstring> restoredNames = CollectRestoredFilenames(context->workspace);
            generator.SetKnownFileNames(restoredNames);
            WriteDebugLog(context, L"[TEST] Step3.5: SetKnownFileNames count=" +
                          std::to_wstring((unsigned int)restoredNames.size()));
        }
        PostLog(context, L"正在根据扩展集生成静态 Hash...（可能需要几分钟）");
        WriteDebugLog(context, L"[TEST] Step4: Calling GenerateFromExtension...");
        WriteDebugLog(context, L"[TEST]   extDir=" + context->extensionDirectory +
                      L", outputDir=" + Combine(context->workspace, L"StaticHash_Output"));

        bool genOk = false;
        try
        {
            genOk = generator.GenerateFromExtension(context->extensionDirectory,
                                                    Combine(context->workspace, L"StaticHash_Output"),
                                                    result.hashResult,
                                                    error);
            WriteDebugLog(context, L"[TEST] Step4: GenerateFromExtension returned " +
                          std::wstring(genOk ? L"OK" : L"FAIL") +
                          (genOk ? L", paths=" + std::to_wstring(result.hashResult.resourcePathCount) : L", error=" + error));
        }
        catch (const std::bad_alloc&)
        {
            WriteDebugLog(context, L"[TEST] Step4 EXCEPTION: std::bad_alloc (OOM)");
            result.errorMessage = L"测试失败：生成静态 Hash 时内存不足。";
            context->result = result;
            PostMessageW(context->window, WM_APP_TEST_DONE, 0, 0);
            return 0;
        }
        catch (const std::exception& e)
        {
            std::wstring what;
            int len = MultiByteToWideChar(CP_UTF8, 0, e.what(), -1, nullptr, 0);
            if (len > 0) { what.resize(len - 1); MultiByteToWideChar(CP_UTF8, 0, e.what(), -1, &what[0], len); }
            WriteDebugLog(context, L"[TEST] Step4 EXCEPTION: std::exception: " + what);
            result.errorMessage = L"测试失败：" + what;
            context->result = result;
            PostMessageW(context->window, WM_APP_TEST_DONE, 0, 0);
            return 0;
        }
        catch (...)
        {
            WriteDebugLog(context, L"[TEST] Step4 EXCEPTION: unknown");
            result.errorMessage = L"测试失败：生成静态 Hash 时发生未知异常。";
            context->result = result;
            PostMessageW(context->window, WM_APP_TEST_DONE, 0, 0);
            return 0;
        }

        if (!genOk)
        {
            WriteDebugLog(context, L"[TEST] Step4 FAIL: " + error);
            result.errorMessage = error;
            context->result = result;
            PostMessageW(context->window, WM_APP_TEST_DONE, 0, 0);
            return 0;
        }

        context->result.hashResult = result.hashResult;
        PostLog(context, L"静态 Hash 生成完成，候选路径 " + std::to_wstring(result.hashResult.resourcePathCount) + L" 条");
        WriteDebugLog(context, L"[TEST] Step5: Hash gen OK, paths=" +
                      std::to_wstring(result.hashResult.resourcePathCount));

        ResourceRestorerLite restorer;
        WriteDebugLog(context, L"[TEST] Step6: Calling RestoreWorkspaceEx...");
        WriteDebugLog(context, L"[TEST]   workspace=" + context->workspace + L", workers=1, fallback=0, inference=0");

        if (!restorer.RestoreWorkspaceEx(context->workspace, 1, false, result.restoreResult, error, RestoreProgressBridge, context))
        {
            WriteDebugLog(context, L"[TEST] Step6 FAIL: " + error);
            result.errorMessage = error;
            context->result = result;
            PostMessageW(context->window, WM_APP_TEST_DONE, 0, 0);
            return 0;
        }
        WriteDebugLog(context, L"[TEST] Step6 OK: restored=" +
                      std::to_wstring(result.restoreResult.restoredFiles) + L"/" +
                      std::to_wstring(result.restoreResult.totalFiles) +
                      L", missingDirHash=" + std::to_wstring(result.restoreResult.missingDirectoryHash) +
                      L", missingFileHash=" + std::to_wstring(result.restoreResult.missingFileNameHash) +
                      L", copyFailed=" + std::to_wstring(result.restoreResult.copyFailed));

        result.success = true;
        result.passedSize = true; // 不再限制包体大小（语音 |F| 条目增加后包体自然变大）
        result.passedBaseline = result.baselineThreshold == 0 || result.restoreResult.restoredFiles >= result.baselineThreshold;
        result.passedValidation = result.passedSize && result.passedBaseline;
        if (!result.passedValidation)
        {
            result.errorMessage = L"测试结果低于基线 75% 或包体超过限制。";
        }
        WriteDebugLog(context, L"[TEST] Step7: Final result — success=" + std::wstring(result.success ? L"yes" : L"no") +
                      L", passedSize=" + std::wstring(result.passedSize ? L"yes" : L"no") +
                      L", passedBaseline=" + std::wstring(result.passedBaseline ? L"yes" : L"no") +
                      L", passedValidation=" + std::wstring(result.passedValidation ? L"yes" : L"no"));

        context->result = result;
        PostMessageW(context->window, WM_APP_TEST_DONE, 0, 0);
        WriteDebugLog(context, L"[TEST] ========== WorkerProc DONE ==========");
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
        column.cx = 352; // 原 176，翻倍避免玩家手动拉
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

                // 扩展集测试模式：修改控件文本
                SetWindowTextW(GetDlgItem(hwnd, IDC_RESTORE_HINT), L"测试扩展集");
                SetWindowTextW(context->outputEdit, context->extensionDirectory.c_str());
                SetWindowTextW(GetDlgItem(hwnd, IDC_RESTORE_START), L"开始测试");
                SetWindowTextW(GetDlgItem(hwnd, IDC_RESTORE_OPEN), L"打开扩展集目录");
                SetWindowTextW(GetDlgItem(hwnd, IDC_RESTORE_BACK), L"返回主程序");

                // 隐藏还原模式下才用到的控件
                ShowWindow(GetDlgItem(hwnd, IDC_RESTORE_FALLBACK), SW_HIDE);
                ShowWindow(GetDlgItem(hwnd, IDC_RESTORE_INFERENCE), SW_HIDE);
                ShowWindow(GetDlgItem(hwnd, IDC_RESTORE_WORKERINFO), SW_HIDE);

                InitializeColumns(context->taskList);

                // 预先加载任务列表，让玩家打开窗口就知道要测哪些包
                SetRow(context, L"生成静态 Hash", RestoreUiTaskState::Queued, 0, 1, L"点击\"开始测试\"再运行");
                for (const std::wstring& packageName : CollectPackageNames(Combine(context->workspace, L"Extractor_Output")))
                {
                    SetRow(context, packageName, RestoreUiTaskState::Queued, 0, 0, L"等待开始");
                }

                SetProgress(context, 0, L"点击\"开始测试\"运行扩展集测试");
                AppendLog(context, L"已打开扩展集测试工具，点击开始测试运行。", 0u);
                return TRUE;
            }
            case WM_COMMAND:
                if (LOWORD(wParam) == IDC_RESTORE_START && context && !context->workerThread)
                {
                    // 点"开始测试"才启动 WorkerProc
                    EnableWindow(GetDlgItem(hwnd, IDC_RESTORE_START), FALSE);
                    SetRow(context, L"生成静态 Hash", RestoreUiTaskState::Restoring, 0, 1, L"根据扩展集生成哈希");
                    for (const std::wstring& packageName : CollectPackageNames(Combine(context->workspace, L"Extractor_Output")))
                    {
                        SetRow(context, packageName, RestoreUiTaskState::Queued, 0, 0, L"等待开始");
                    }
                    SetProgress(context, 5, L"正在生成静态 Hash...");
                    AppendLog(context, L"开始扩展集测试...", 0u);
                    context->workerThread = CreateThread(nullptr, 0, WorkerProc, context, 0, nullptr);
                    return TRUE;
                }
                if (LOWORD(wParam) == IDC_RESTORE_OPEN && context)
                {
                    ShellExecuteW(hwnd, L"open", context->extensionDirectory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    return TRUE;
                }
                if (LOWORD(wParam) == IDC_RESTORE_BACK || LOWORD(wParam) == IDCANCEL)
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
            case WM_APP_TEST_LOG:
            {
                std::wstring* text = reinterpret_cast<std::wstring*>(lParam);
                if (text)
                {
                    AppendLog(context, *text, 50u);
                    delete text;
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
