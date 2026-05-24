#include "ResourceRestorerLite.h"

#include <windows.h>

#include <atomic>
#include <cwchar>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr const wchar_t HashLogSplit[] = L"##YSig##";

    struct PackageResult
    {
        unsigned int totalFiles = 0;
        unsigned int restoredFiles = 0;
        unsigned int missingDirectoryHash = 0;
        unsigned int missingFileNameHash = 0;
        unsigned int copyFailed = 0;
        bool fatalError = false;
    };

    struct PackageTask
    {
        std::wstring packageDirectory;
        std::wstring packageName;
        unsigned int estimatedFiles = 0;
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

    bool DirectoryExists(const std::wstring& path)
    {
        DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    std::wstring DirectoryName(const std::wstring& path)
    {
        size_t pos = path.find_last_of(L"\\/");
        return pos == std::wstring::npos ? L"" : path.substr(0, pos);
    }

    bool HasChildDirectory(const std::wstring& path)
    {
        if (!DirectoryExists(path)) return false;
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileW(Combine(path, L"*").c_str(), &data);
        if (find == INVALID_HANDLE_VALUE) return false;
        bool found = false;
        do
        {
            if (IsDotEntry(data.cFileName)) continue;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                found = true;
                break;
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
        return found;
    }

    void EnsureDirectory(const std::wstring& path)
    {
        if (path.empty() || DirectoryExists(path)) return;
        size_t split = path.find_last_of(L"\\/");
        if (split != std::wstring::npos) EnsureDirectory(path.substr(0, split));
        CreateDirectoryW(path.c_str(), nullptr);
    }

    std::wstring TrimLine(std::wstring value)
    {
        while (!value.empty() && (value.back() == L'\r' || value.back() == L'\n'))
        {
            value.pop_back();
        }
        return value;
    }

    std::wstring NormalizeHash(std::wstring hash)
    {
        std::wstring output;
        output.reserve(hash.size());
        for (wchar_t ch : hash)
        {
            if (ch >= L'a' && ch <= L'f') output.push_back(ch - L'a' + L'A');
            else if ((ch >= L'A' && ch <= L'F') || (ch >= L'0' && ch <= L'9')) output.push_back(ch);
        }
        return output;
    }

    bool IsUnsafeRestoredPath(const std::wstring& path)
    {
        if (path.size() >= 2 && path[1] == L':') return true;
        if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') return true;
        size_t start = 0;
        while (start <= path.size())
        {
            size_t end = path.find_first_of(L"\\/", start);
            std::wstring part = path.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
            if (part == L"..") return true;
            if (end == std::wstring::npos) break;
            start = end + 1;
        }
        return false;
    }

    bool ReadUtf16File(const std::wstring& path, std::wstring& output)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 0x7ffffffe)
        {
            CloseHandle(file);
            return false;
        }
        std::vector<unsigned char> buffer((size_t)size.QuadPart);
        DWORD read = 0;
        bool success = ReadFile(file, buffer.data(), (DWORD)buffer.size(), &read, nullptr) && read == buffer.size();
        CloseHandle(file);
        if (!success || buffer.size() < sizeof(wchar_t)) return false;
        size_t offset = 0;
        if (buffer.size() >= 2 && buffer[0] == 0xff && buffer[1] == 0xfe) offset = 2;
        output.assign((const wchar_t*)(buffer.data() + offset), (buffer.size() - offset) / sizeof(wchar_t));
        return true;
    }

    bool LoadHashMap(const std::wstring& path, std::unordered_map<std::wstring, std::wstring>& output)
    {
        std::wstring content;
        if (!ReadUtf16File(path, content)) return false;
        size_t start = 0;
        while (start < content.size())
        {
            size_t end = content.find(L'\n', start);
            std::wstring line = TrimLine(content.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
            size_t split = line.rfind(HashLogSplit);
            if (split != std::wstring::npos)
            {
                std::wstring text = line.substr(0, split);
                std::wstring hash = NormalizeHash(line.substr(split + wcslen(HashLogSplit)));
                if (!hash.empty() && !text.empty() && !IsUnsafeRestoredPath(text))
                {
                    if (text == L"%EmptyString%") text.clear();
                    output[hash] = text;
                }
            }
            if (end == std::wstring::npos) break;
            start = end + 1;
        }
        return true;
    }

    void WriteUtf16File(const std::wstring& path, const std::wstring& content)
    {
        EnsureDirectory(path.substr(0, path.find_last_of(L"\\/")));
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        WORD bom = 0xfeff;
        DWORD written = 0;
        WriteFile(file, &bom, sizeof(bom), &written, nullptr);
        if (!content.empty()) WriteFile(file, content.data(), (DWORD)(content.size() * sizeof(wchar_t)), &written, nullptr);
        CloseHandle(file);
    }

    std::wstring FormatPercent(unsigned int value, unsigned int total)
    {
        if (total == 0) return L"0.00%";
        unsigned int scaled = (unsigned int)(((unsigned long long)value * 10000ull + total / 2u) / total);
        wchar_t text[32]{};
        wsprintfW(text, L"%u.%02u%%", scaled / 100u, scaled % 100u);
        return text;
    }

    std::wstring GetCurrentLocalTimeString()
    {
        SYSTEMTIME st;
        ::GetLocalTime(&st);
        wchar_t buf[64]{};
        ::wsprintfW(buf, L"%04u-%02u-%02u %02u:%02u:%02u",
                    st.wYear, st.wMonth, st.wDay,
                    st.wHour, st.wMinute, st.wSecond);
        return buf;
    }

    void NotifyRestoreProgress(RestoreProgressCallback callback,
                               void* context,
                               const std::wstring& packageName,
                               RestoreUiTaskState state,
                               unsigned int current,
                               unsigned int total,
                               const std::wstring& detail)
    {
        if (!callback)
        {
            return;
        }

        RestoreProgressInfo info{};
        info.packageName = packageName;
        info.state = state;
        info.current = current;
        info.total = total;
        info.detail = detail;
        callback(info, context);
    }

    unsigned int CountPackageFiles(const std::wstring& packageDirectory)
    {
        unsigned int count = 0;
        WIN32_FIND_DATAW directoryData{};
        HANDLE directoryFind = FindFirstFileW(Combine(packageDirectory, L"*").c_str(), &directoryData);
        if (directoryFind == INVALID_HANDLE_VALUE)
        {
            return 0;
        }

        do
        {
            if (IsDotEntry(directoryData.cFileName) || (directoryData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                continue;
            }

            std::wstring hashDirectory = Combine(packageDirectory, directoryData.cFileName);
            WIN32_FIND_DATAW fileData{};
            HANDLE fileFind = FindFirstFileW(Combine(hashDirectory, L"*").c_str(), &fileData);
            if (fileFind == INVALID_HANDLE_VALUE)
            {
                continue;
            }

            do
            {
                if (!IsDotEntry(fileData.cFileName) && (fileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                {
                    ++count;
                }
            } while (FindNextFileW(fileFind, &fileData));
            FindClose(fileFind);
        } while (FindNextFileW(directoryFind, &directoryData));
        FindClose(directoryFind);
        return count;
    }

    std::vector<PackageTask> CollectPackageTasks(const std::wstring& extractorOutput)
    {
        std::vector<PackageTask> tasks;
        WIN32_FIND_DATAW packageData{};
        HANDLE packageFind = FindFirstFileW(Combine(extractorOutput, L"*").c_str(), &packageData);
        if (packageFind == INVALID_HANDLE_VALUE)
        {
            return tasks;
        }

        do
        {
            if (IsDotEntry(packageData.cFileName) || (packageData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                continue;
            }

            PackageTask task{};
            task.packageName = packageData.cFileName;
            task.packageDirectory = Combine(extractorOutput, packageData.cFileName);
            task.estimatedFiles = CountPackageFiles(task.packageDirectory);
            tasks.push_back(task);
        } while (FindNextFileW(packageFind, &packageData));
        FindClose(packageFind);
        return tasks;
    }

    void AddPackageResult(const PackageResult& source, ResourceRestorerLite::Result& target)
    {
        target.totalFiles += source.totalFiles;
        target.restoredFiles += source.restoredFiles;
        target.missingDirectoryHash += source.missingDirectoryHash;
        target.missingFileNameHash += source.missingFileNameHash;
        target.copyFailed += source.copyFailed;
    }

    void RestorePackage(const PackageTask& task,
                        const std::wstring& outputRoot,
                        const std::unordered_map<std::wstring, std::wstring>& directoryMap,
                        const std::unordered_map<std::wstring, std::wstring>& fileNameMap,
                        PackageResult& result,
                        RestoreProgressCallback progressCallback,
                        void* progressContext)
    {
        NotifyRestoreProgress(progressCallback, progressContext, task.packageName, RestoreUiTaskState::Restoring, 0u, task.estimatedFiles, L"开始还原");

        WIN32_FIND_DATAW directoryData{};
        HANDLE directoryFind = FindFirstFileW(Combine(task.packageDirectory, L"*").c_str(), &directoryData);
        if (directoryFind == INVALID_HANDLE_VALUE)
        {
            result.fatalError = true;
            NotifyRestoreProgress(progressCallback, progressContext, task.packageName, RestoreUiTaskState::Failed, 0u, 0u, L"封包目录无效");
            return;
        }

        unsigned int processed = 0u;
        do
        {
            if (IsDotEntry(directoryData.cFileName) || (directoryData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                continue;
            }

            std::wstring directoryHash = NormalizeHash(directoryData.cFileName);
            auto directoryIt = directoryMap.find(directoryHash);
            std::wstring hashDirectory = Combine(task.packageDirectory, directoryData.cFileName);
            WIN32_FIND_DATAW fileData{};
            HANDLE fileFind = FindFirstFileW(Combine(hashDirectory, L"*").c_str(), &fileData);
            if (fileFind == INVALID_HANDLE_VALUE)
            {
                continue;
            }

            do
            {
                if (IsDotEntry(fileData.cFileName) || (fileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                {
                    continue;
                }

                ++result.totalFiles;
                ++processed;
                std::wstring detail;

                if (directoryIt == directoryMap.end())
                {
                    ++result.missingDirectoryHash;
                    detail = L"缺少目录 Hash: " + directoryHash;
                    NotifyRestoreProgress(progressCallback, progressContext, task.packageName, RestoreUiTaskState::Restoring, processed, task.estimatedFiles, detail);
                    continue;
                }

                std::wstring fileHash = NormalizeHash(fileData.cFileName);
                auto fileIt = fileNameMap.find(fileHash);
                if (fileIt == fileNameMap.end())
                {
                    ++result.missingFileNameHash;
                    detail = L"缺少文件名 Hash: " + fileHash;
                    NotifyRestoreProgress(progressCallback, progressContext, task.packageName, RestoreUiTaskState::Restoring, processed, task.estimatedFiles, detail);
                    continue;
                }

                std::wstring packageOutput = Combine(outputRoot, task.packageName);
                std::wstring outputDirectory = directoryIt->second.empty() ? packageOutput : Combine(packageOutput, directoryIt->second);
                EnsureDirectory(outputDirectory);

                std::wstring sourcePath = Combine(hashDirectory, fileData.cFileName);
                std::wstring targetPath = Combine(outputDirectory, fileIt->second);
                if (!CopyFileW(sourcePath.c_str(), targetPath.c_str(), FALSE))
                {
                    ++result.copyFailed;
                    detail = L"复制失败: " + fileIt->second;
                    NotifyRestoreProgress(progressCallback, progressContext, task.packageName, RestoreUiTaskState::Restoring, processed, task.estimatedFiles, detail);
                    continue;
                }

                ++result.restoredFiles;
                detail = task.packageName + L"\\" + directoryIt->second + L"\\" + fileIt->second;
                NotifyRestoreProgress(progressCallback, progressContext, task.packageName, RestoreUiTaskState::Restoring, processed, task.estimatedFiles, detail);
            } while (FindNextFileW(fileFind, &fileData));
            FindClose(fileFind);
        } while (FindNextFileW(directoryFind, &directoryData));
        FindClose(directoryFind);

        if (result.totalFiles == 0)
        {
            result.fatalError = true;
            NotifyRestoreProgress(progressCallback, progressContext, task.packageName, RestoreUiTaskState::Failed, 0u, 0u, L"没有可处理的 hash 文件");
            return;
        }

        std::wstring summary = L"成功 " + std::to_wstring(result.restoredFiles)
                             + L" / 总数 " + std::to_wstring(result.totalFiles)
                             + L" / 成功率 " + FormatPercent(result.restoredFiles, result.totalFiles);
        NotifyRestoreProgress(progressCallback, progressContext, task.packageName, RestoreUiTaskState::Completed, result.restoredFiles, result.totalFiles, summary);
    }
}

bool ResourceRestorerLite::RestoreWorkspace(const std::wstring& workspace,
                                            Result& result,
                                            std::wstring& errorMessage)
{
    return RestoreWorkspaceEx(workspace, 1u, result, errorMessage, nullptr, nullptr);
}

bool ResourceRestorerLite::RestoreWorkspaceEx(const std::wstring& workspace,
                                              unsigned int workerCount,
                                              Result& result,
                                              std::wstring& errorMessage,
                                              RestoreProgressCallback progressCallback,
                                              void* progressContext)
{
    result = Result{};
    errorMessage.clear();

    std::wstring extractorOutput = Combine(workspace, L"Extractor_Output");
    std::wstring staticHashOutput = Combine(workspace, L"StaticHash_Output");
    std::wstring dynamicHashOutput = Combine(workspace, L"StringHashDumper_Output");
    std::wstring restoredOutput = Combine(workspace, L"Restored_Extractor_Output");

    std::wstring userDirectory = DirectoryName(workspace);
    std::wstring gameDirectory = DirectoryName(userDirectory);
    if (!HasChildDirectory(extractorOutput) && HasChildDirectory(Combine(gameDirectory, L"Extractor_Output")))
    {
        extractorOutput = Combine(gameDirectory, L"Extractor_Output");
    }
    if (!DirectoryExists(dynamicHashOutput) && DirectoryExists(Combine(gameDirectory, L"StringHashDumper_Output")))
    {
        dynamicHashOutput = Combine(gameDirectory, L"StringHashDumper_Output");
    }

    if (!DirectoryExists(extractorOutput))
    {
        errorMessage = L"找不到 Extractor_Output: " + extractorOutput;
        return false;
    }

    std::unordered_map<std::wstring, std::wstring> directoryMap;
    std::unordered_map<std::wstring, std::wstring> fileNameMap;
    if (DirectoryExists(dynamicHashOutput))
    {
        LoadHashMap(Combine(dynamicHashOutput, L"DirectoryHash.log"), directoryMap);
        LoadHashMap(Combine(dynamicHashOutput, L"FileNameHash.log"), fileNameMap);
    }
    if (DirectoryExists(staticHashOutput))
    {
        LoadHashMap(Combine(staticHashOutput, L"DirectoryHash.log"), directoryMap);
        LoadHashMap(Combine(staticHashOutput, L"FileNameHash.log"), fileNameMap);
    }

    if (directoryMap.empty() || fileNameMap.empty())
    {
        errorMessage = L"Hash 映射为空，无法还原。";
        return false;
    }

    EnsureDirectory(restoredOutput);
    std::vector<PackageTask> tasks = CollectPackageTasks(extractorOutput);
    if (tasks.empty())
    {
        errorMessage = L"Extractor_Output 中没有可处理目录。";
        return false;
    }

    for (const PackageTask& task : tasks)
    {
        NotifyRestoreProgress(progressCallback, progressContext, task.packageName, RestoreUiTaskState::Queued, 0u, task.estimatedFiles, restoredOutput);
    }

    if (workerCount == 0u)
    {
        workerCount = 1u;
    }
    if (workerCount > tasks.size())
    {
        workerCount = (unsigned int)tasks.size();
    }

    std::atomic<size_t> nextIndex{ 0u };
    std::mutex resultLock;
    std::map<std::wstring, PackageResult> packageResults;
    std::vector<std::thread> workers;
    workers.reserve(workerCount);

    for (unsigned int worker = 0u; worker < workerCount; ++worker)
    {
        workers.emplace_back([&]()
        {
            while (true)
            {
                size_t index = nextIndex.fetch_add(1u);
                if (index >= tasks.size())
                {
                    break;
                }

                PackageResult packageResult{};
                RestorePackage(tasks[index], restoredOutput, directoryMap, fileNameMap, packageResult, progressCallback, progressContext);

                std::lock_guard<std::mutex> lock(resultLock);
                AddPackageResult(packageResult, result);
                packageResults[tasks[index].packageName] = packageResult;
            }
        });
    }

    for (std::thread& worker : workers)
    {
        worker.join();
    }

    if (result.totalFiles == 0)
    {
        errorMessage = L"Extractor_Output 中没有找到 hash 文件。";
        return false;
    }

    result.reportPath = Combine(restoredOutput, L"RestoreReport.txt");
    std::wstring report;
    report += L"资源文件名还原报告\r\n";
    report += L"还原完成时间: " + GetCurrentLocalTimeString() + L"\r\n\r\n";
    report += L"工作区: " + workspace + L"\r\n";
    report += L"工作线程: " + std::to_wstring(workerCount) + L"\r\n";
    report += L"总文件数: " + std::to_wstring(result.totalFiles) + L"\r\n";
    report += L"成功还原: " + std::to_wstring(result.restoredFiles) + L"\r\n";
    report += L"成功率: " + FormatPercent(result.restoredFiles, result.totalFiles) + L"\r\n";
    report += L"缺少目录 Hash: " + std::to_wstring(result.missingDirectoryHash) + L"\r\n";
    report += L"缺少文件名 Hash: " + std::to_wstring(result.missingFileNameHash) + L"\r\n";
    report += L"复制失败: " + std::to_wstring(result.copyFailed) + L"\r\n";
    report += L"\r\n--- 各目录还原情况 ---\r\n";
    for (const auto& entry : packageResults)
    {
        report += entry.first + L"│" + std::to_wstring(entry.second.restoredFiles)
               + L"/" + std::to_wstring(entry.second.totalFiles)
               + L"│" + FormatPercent(entry.second.restoredFiles, entry.second.totalFiles) + L"\r\n";
    }
    WriteUtf16File(result.reportPath, report);
    return true;
}
