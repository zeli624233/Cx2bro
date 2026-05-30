#include "ResourceRestorerLite.h"
#include "StaticHashGeneratorLite.h"

#include <windows.h>

#include <atomic>
#include <cwchar>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr const wchar_t HashLogSplit[] = L"##YSig##";

    struct PackageStats
    {
        unsigned int totalFiles = 0;
        unsigned int restoredFiles = 0;
        unsigned int missingDirectoryHash = 0;
        unsigned int missingFileNameHash = 0;
        unsigned int copyFailed = 0;
        unsigned int fallbackRestoredFiles = 0;
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

    // Forward declarations for functions used across the file
    std::wstring DetectExtension(const std::wstring& filePath);

    std::wstring TrimLine(std::wstring value)
    {
        while (!value.empty() && (value.back() == L'\r' || value.back() == L'\n' || value.back() == L' '))
        {
            value.pop_back();
        }
        while (!value.empty() && (value.front() == L' '))
        {
            value.erase(value.begin());
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

    void AddPackageStats(const PackageStats& source, ResourceRestorerLite::Result& target)
    {
        target.totalFiles += source.totalFiles;
        target.restoredFiles += source.restoredFiles;
        target.missingDirectoryHash += source.missingDirectoryHash;
        target.missingFileNameHash += source.missingFileNameHash;
        target.copyFailed += source.copyFailed;
        target.fallbackRestoredFiles += source.fallbackRestoredFiles;
    }

    void RestorePackage(const PackageTask& task,
                        const std::wstring& outputRoot,
                        const std::unordered_map<std::wstring, std::wstring>& directoryMap,
                        const std::unordered_map<std::wstring, std::wstring>& fileNameMap,
                        PackageStats& result,
                        bool enableFallback,
                        RestoreProgressCallback progressCallback,
                        void* progressContext)
    {
        NotifyRestoreProgress(progressCallback, progressContext, task.packageName, RestoreUiTaskState::Restoring, 0u, task.estimatedFiles, L"开始还原");

        std::set<std::wstring> processedFallbackDirs; // 已处理过的 hash 目录，避免重复计数

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
                    if (enableFallback) {
                        // Directory hash unknown — place files in hash-named dir under the package
                        // 同一个 hash 目录只复制一次，避免嵌套循环重复计数
                        if (processedFallbackDirs.find(directoryData.cFileName) == processedFallbackDirs.end())
                        {
                            processedFallbackDirs.insert(directoryData.cFileName);
                            std::wstring pkgOutput = Combine(outputRoot, task.packageName);
                            EnsureDirectory(pkgOutput);
                            std::wstring dstDir = Combine(pkgOutput, directoryData.cFileName);
                            EnsureDirectory(dstDir);
                            WIN32_FIND_DATAW fbfd;
                            HANDLE fbf = FindFirstFileW(Combine(hashDirectory, L"*").c_str(), &fbfd);
                            if (fbf != INVALID_HANDLE_VALUE) {
                                do {
                                    if (!IsDotEntry(fbfd.cFileName) && (fbfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                                        std::wstring srcPath = Combine(hashDirectory, fbfd.cFileName);
                                        std::wstring ext = DetectExtension(srcPath);
                                        std::wstring dstName = fbfd.cFileName + ext;
                                        std::wstring dstPath = Combine(dstDir, dstName);
                                        if (CopyFileW(srcPath.c_str(), dstPath.c_str(), FALSE)) {
                                            ++result.fallbackRestoredFiles;
                                        }
                                    }
                                } while (FindNextFileW(fbf, &fbfd));
                                FindClose(fbf);
                            }
                        }
                    }
                    detail = L"目录未知，按后缀保留到 " + std::wstring(directoryData.cFileName);
                    NotifyRestoreProgress(progressCallback, progressContext, task.packageName, RestoreUiTaskState::Restoring, processed, task.estimatedFiles, detail);
                    continue;
                }

                std::wstring fileHash = NormalizeHash(fileData.cFileName);
                auto fileIt = fileNameMap.find(fileHash);
                if (fileIt == fileNameMap.end())
                {
                    ++result.missingFileNameHash;
                    if (enableFallback) {
                        // File hash unknown but directory IS known — place in proper directory with detected extension
                        std::wstring pkgOutput = Combine(outputRoot, task.packageName);
                        std::wstring outputDir = directoryIt->second.empty() ? pkgOutput : Combine(pkgOutput, directoryIt->second);
                        EnsureDirectory(outputDir);
                        std::wstring srcPath = Combine(hashDirectory, fileData.cFileName);
                        std::wstring ext = DetectExtension(srcPath);
                        std::wstring dstPath = Combine(outputDir, std::wstring(fileData.cFileName) + ext);
                        if (CopyFileW(srcPath.c_str(), dstPath.c_str(), FALSE)) {
                            ++result.fallbackRestoredFiles;
                        }
                    }
                    detail = L"文件名未知，按后缀保留到 " + directoryIt->second + L"\\" + std::wstring(fileData.cFileName);
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
        NotifyRestoreProgress(progressCallback, progressContext, task.packageName, RestoreUiTaskState::Completed, result.totalFiles, result.totalFiles, summary);
    }

    // =====================================================
    // Secondary Inference Helper Functions
    // =====================================================

    // Detect file extension from magic bytes (signature-based)
    // Based on GARbro-Mod analysis and Krkr format knowledge
    std::wstring DetectExtension(const std::wstring& filePath)
    {
        HANDLE file = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return L"";

        uint8_t header[32]{};
        DWORD read = 0;
        bool ok = ReadFile(file, header, sizeof(header), &read, nullptr) && read >= 2;
        CloseHandle(file);
        if (!ok) return L"";

        // Check signatures in priority order (most specific first)

        // --- Image formats ---
        if (read >= 4 && header[0] == 0x89 && header[1] == 0x50 && header[2] == 0x4E && header[3] == 0x47) return L".png";
        if (read >= 2 && header[0] == 0xFF && header[1] == 0xD8) return L".jpg";
        if (read >= 4 && header[0] == 0x47 && header[1] == 0x49 && header[2] == 0x46 && header[3] == 0x38) return L".gif";
        if (read >= 2 && header[0] == 0x42 && header[1] == 0x4D) return L".bmp";
        if (read >= 4 && header[0] == 0x38 && header[1] == 0x42 && header[2] == 0x50 && header[3] == 0x53) return L".psd";
        if (read >= 4 && header[0] == 0x44 && header[1] == 0x44 && header[2] == 0x53 && header[3] == 0x20) return L".dds";
        if (read >= 4 && header[0] == 0x49 && header[1] == 0x49 && header[2] == 0x2A && header[3] == 0x00) return L".tif";
        if (read >= 4 && header[0] == 0x4D && header[1] == 0x4D && header[2] == 0x00 && header[3] == 0x2A) return L".tif";

        // --- Krkr image formats ---
        if (read >= 4 && ((header[0] == 0x54 && header[1] == 0x4C && header[2] == 0x47 && (header[3] == 0x30 || header[3] == 0x35 || header[3] == 0x36)))) return L".tlg";
        if (read >= 4 && header[0] == 0x54 && header[1] == 0x56 && header[2] == 0x50 && header[3] == 0x20) return L".tvp";

        // --- Krkr script formats ---
        if (read >= 6 && header[0] == 0x54 && header[1] == 0x4A && header[2] == 0x53 && header[3] == 0x32 && header[4] == 0x31 && header[5] == 0x30) return L".tjs";
        if (read >= 4 && ((header[0] == 0x54 && header[1] == 0x4A && header[2] == 0x53 && header[3] == 0x32) ||
                          (header[0] == 0x54 && header[1] == 0x4A && header[2] == 0x53 && header[3] == 0x2F))) return L".tjs";
        if (read >= 6 && header[0] == 0x23 && header[1] == 0x32 && header[2] == 0x2E && header[3] == 0x30 && header[4] == 0x30 && header[5] == 0x0A) return L".sli";

        // --- Font formats ---
        if (read >= 4 && header[0] == 0x4F && header[1] == 0x54 && header[2] == 0x54 && header[3] == 0x4F) return L".otf";
        if (read >= 4 && header[0] == 0x00 && header[1] == 0x01 && header[2] == 0x00 && header[3] == 0x00) return L".ttf";
        if (read >= 4 && header[0] == 0x6D && header[1] == 0x64 && header[2] == 0x66 && header[3] == 0x00) return L".mdf";

        // --- Audio formats ---
        if (read >= 4 && header[0] == 0x4F && header[1] == 0x67 && header[2] == 0x67 && header[3] == 0x53) return L".ogg";
        if (read >= 4 && header[0] == 0x66 && header[1] == 0x4C && header[2] == 0x61 && header[3] == 0x43) return L".flac";
        if (read >= 4 && header[0] == 0x4B && header[1] == 0x42 && header[2] == 0x41 && header[3] == 0x44) return L".kad";
        if (read >= 3 && header[0] == 0x49 && header[1] == 0x44 && header[2] == 0x33) return L".mp3";

        // --- WAV: RIFF + WAVE ---
        if (read >= 12 && header[0] == 0x52 && header[1] == 0x49 && header[2] == 0x46 && header[3] == 0x46 &&
                        header[8] == 0x57 && header[9] == 0x41 && header[10] == 0x56 && header[11] == 0x45) return L".wav";
        // RIFF (other) → .ogg fallback
        if (read >= 4 && header[0] == 0x52 && header[1] == 0x49 && header[2] == 0x46 && header[3] == 0x46) return L".ogg";

        // --- Video formats ---
        if (read >= 4 && header[0] == 0x1A && header[1] == 0x45 && header[2] == 0xDF && header[3] == 0xA3) return L".mkv";
        if (read >= 4 && header[0] == 0x41 && header[1] == 0x4A && header[2] == 0x50 && header[3] == 0x4D) return L".mjp";
        if (read >= 16 && header[0] == 0x30 && header[1] == 0x26 && header[2] == 0xB2 && header[3] == 0x75) return L".wmv";
        if (read >= 8 && header[4] == 0x66 && header[5] == 0x74 && header[6] == 0x79 && header[7] == 0x70) return L".mp4";

        // --- PSB / Emote ---
        if (read >= 3 && header[0] == 0x50 && header[1] == 0x53 && header[2] == 0x42) return L".psb";

        // --- PE executable ---
        if (read >= 2 && header[0] == 0x4D && header[1] == 0x5A) return L".dll";

        // --- Archive formats ---
        if (read >= 4 && header[0] == 0x50 && header[1] == 0x4B && header[2] == 0x03 && header[3] == 0x04) return L".zip";
        if (read >= 5 && header[0] == 0x58 && header[1] == 0x50 && header[2] == 0x4B && header[3] == 0x31 && header[4] == 0x1A) return L".xpk";

        // --- UTF-16LE text (Krkr scripts: .ks, .tjs, .asd, .stand, .func, etc.) ---
        if (read >= 2 && header[0] == 0xFF && header[1] == 0xFE)
        {
            // Check if first real character suggests .ks (semicolon) or .tjs (other)
            if (read >= 6 && (header[4] == L';' || header[4] == L'/')) return L".ks";
            return L".tjs";
        }

        return L"";
    }

    // Extract quoted filename strings from binary data (for compiled TJS bytecode)
    std::vector<std::wstring> ExtractQuotedStringsFromBinary(const std::vector<uint8_t>& data)
    {
        std::vector<std::wstring> results;
        // Scan for ASCII quoted strings: "filename.ext" or 'filename.ext'
        for (size_t i = 0; i + 3 < data.size(); ++i)
        {
            if ((data[i] == '"' || data[i] == '\'') && data[i] != 0)
            {
                uint8_t quote = data[i];
                size_t start = i + 1;
                size_t end = start;
                bool hasDot = false;
                while (end < data.size() && data[end] != quote && data[end] != 0)
                {
                    if (data[end] == '.') hasDot = true;
                    // Only accept printable ASCII
                    if (data[end] < 0x20 || data[end] > 0x7E) break;
                    ++end;
                }
                if (end > start && end < data.size() && data[end] == quote && hasDot)
                {
                    std::wstring str;
                    for (size_t j = start; j < end; ++j)
                        str.push_back((wchar_t)data[j]);
                    if (str.size() >= 4 && str.size() <= 260)
                        results.push_back(str);
                    i = end; // skip past the string
                }
            }
        }
        return results;
    }

    // Try to read a file as text; if UTF-16LE fails, try UTF-8, then binary string extraction
    bool ReadTextFileForStrings(const std::wstring& path, std::vector<std::wstring>& extractedStrings)
    {
        // First try UTF-16LE
        std::wstring content;
        if (ReadUtf16File(path, content))
        {
            // Successfully read as text - use regex extraction
            std::wregex quotedRegex(LR"(["']([^"'\\]+(?:\.[a-zA-Z0-9]+))["'])");
            std::wsregex_iterator it(content.begin(), content.end(), quotedRegex);
            std::wsregex_iterator end;
            for (; it != end; ++it)
            {
                std::wstring s = (*it)[1].str();
                if (s.size() >= 4 && s.size() <= 260 &&
                    s.find(L'/') == std::wstring::npos &&
                    s.find(L'\\') == std::wstring::npos &&
                    s.find_first_of(L"<>:\"|?*") == std::wstring::npos)
                {
                    extractedStrings.push_back(s);
                }
            }
            return true;
        }

        // Try UTF-8
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            LARGE_INTEGER size{};
            if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= 0x1000000)
            {
                std::vector<uint8_t> buf((size_t)size.QuadPart);
                DWORD read = 0;
                if (ReadFile(file, buf.data(), (DWORD)buf.size(), &read, nullptr) && read == buf.size())
                {
                    CloseHandle(file);

                    // Check if it looks like UTF-8 (ASCII or valid UTF-8)
                    bool looksLikeText = true;
                    for (size_t i = 0; i < buf.size() && looksLikeText; ++i)
                    {
                        if (buf[i] >= 0x80 && buf[i] <= 0xBF) looksLikeText = false;
                    }

                    if (looksLikeText)
                    {
                        // Extract ASCII quoted strings from binary data
                        auto strings = ExtractQuotedStringsFromBinary(buf);
                        extractedStrings.insert(extractedStrings.end(), strings.begin(), strings.end());
                    }
                    else
                    {
                        // Binary / compiled bytecode - extract strings from raw bytes
                        auto strings = ExtractQuotedStringsFromBinary(buf);
                        extractedStrings.insert(extractedStrings.end(), strings.begin(), strings.end());
                    }
                    return !extractedStrings.empty();
                }
            }
            CloseHandle(file);
        }

        return false;
    }

    // Detect HashSeed from StaticHashReport.txt or Universal.log
    std::wstring DetectHashSeed(const std::wstring& workspace)
    {
        // Try StaticHash_Output first
        std::wstring reportPath = Combine(Combine(workspace, L"StaticHash_Output"), L"StaticHashReport.txt");
        std::wstring content;
        if (ReadUtf16File(reportPath, content)) {
            const std::wstring markers[] = { L"HashSeed:", L"Hash Seed:", L"HashSeed：", L"Hash Seed：" };
            for (const auto& marker : markers) {
                size_t pos = content.find(marker);
                if (pos == std::wstring::npos) continue;
                size_t start = pos + marker.size();
                size_t end = content.find_first_of(L"\r\n", start);
                std::wstring value = TrimLine(content.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
                if (!value.empty()) return value;
            }
        }
        // Fallback: try StringHashDumper_Output/Universal.log
        std::wstring univPath = Combine(Combine(workspace, L"StringHashDumper_Output"), L"Universal.log");
        if (ReadUtf16File(univPath, content)) {
            const std::wstring markers[] = { L"Hash Seed:", L"HashSeed:", L"Hash Seed：", L"HashSeed：" };
            for (const auto& marker : markers) {
                size_t pos = content.find(marker);
                if (pos == std::wstring::npos) continue;
                size_t start = pos + marker.size();
                size_t end = content.find_first_of(L"\r\n", start);
                std::wstring value = TrimLine(content.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
                if (!value.empty()) return value;
            }
        }
        return L"";
    }

    // Information about a remaining hash-named file
    struct RemainingFileInfo
    {
        std::wstring packageName;
        std::wstring dirHash;
    };

    // Build index of all remaining hash-named files in Extractor_Output
    // key = normalized file hash (64 hex chars uppercase), value = {package, dirHash}
    using RemainingIndex = std::unordered_map<std::wstring, RemainingFileInfo>;

    RemainingIndex BuildRemainingIndex(const std::wstring& extractorOutput)
    {
        RemainingIndex index;
        WIN32_FIND_DATAW pkgData{};
        HANDLE pkgFind = FindFirstFileW(Combine(extractorOutput, L"*").c_str(), &pkgData);
        if (pkgFind == INVALID_HANDLE_VALUE) return index;

        do
        {
            if (IsDotEntry(pkgData.cFileName) || (pkgData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                continue;

            std::wstring pkgDir = Combine(extractorOutput, pkgData.cFileName);
            std::wstring pkgName = pkgData.cFileName;

            WIN32_FIND_DATAW dirData{};
            HANDLE dirFind = FindFirstFileW(Combine(pkgDir, L"*").c_str(), &dirData);
            if (dirFind == INVALID_HANDLE_VALUE) continue;

            do
            {
                if (IsDotEntry(dirData.cFileName) || (dirData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                    continue;

                std::wstring dirHash = NormalizeHash(dirData.cFileName);
                std::wstring hashDir = Combine(pkgDir, dirData.cFileName);

                WIN32_FIND_DATAW fileData{};
                HANDLE fileFind = FindFirstFileW(Combine(hashDir, L"*").c_str(), &fileData);
                if (fileFind == INVALID_HANDLE_VALUE) continue;

                do
                {
                    if (IsDotEntry(fileData.cFileName) || (fileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                        continue;

                    std::wstring fileHash = NormalizeHash(fileData.cFileName);
                    if (!fileHash.empty() && fileHash.size() >= 8)
                    {
                        index[fileHash] = { pkgName, dirHash };
                    }
                } while (FindNextFileW(fileFind, &fileData));
                FindClose(fileFind);
            } while (FindNextFileW(dirFind, &dirData));
            FindClose(dirFind);
        } while (FindNextFileW(pkgFind, &pkgData));
        FindClose(pkgFind);

        return index;
    }

    // Pattern extracted from a restored filename
    struct NamePattern
    {
        std::wstring prefix;      // e.g. "bg", "yui"
        std::wstring letter;      // e.g. "a" for bg001a.png, "" for yui0000.ogg
        std::wstring suffix;      // e.g. ".png", ".ogg", ".ogg.sli"
        int numDigits = 0;        // e.g. 4 for "0001"
        int maxNumber = -1;       // highest number observed
        std::vector<std::wstring> numberStrs; // actual number strings from restored files
    };

    // Group key for pattern aggregation: (package, directory, prefix, suffix)
    struct PatternGroupKey
    {
        std::wstring packageName;
        std::wstring directoryName;
        std::wstring prefix;
        std::wstring letter;
        std::wstring suffix;

        bool operator==(const PatternGroupKey& other) const
        {
            return packageName == other.packageName
                && directoryName == other.directoryName
                && prefix == other.prefix
                && letter == other.letter
                && suffix == other.suffix;
        }
    };

    struct PatternGroupKeyHash
    {
        size_t operator()(const PatternGroupKey& key) const
        {
            size_t h = std::hash<std::wstring>{}(key.packageName);
            h ^= std::hash<std::wstring>{}(key.directoryName) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<std::wstring>{}(key.prefix) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<std::wstring>{}(key.letter) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<std::wstring>{}(key.suffix) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };

    // Phase 1: Pattern Expansion
    // Extract naming patterns from restored files, expand range, compute hashes, match leftovers
    int PatternExpandRestore(const std::wstring& restoredOutput,
                             const std::wstring& extractorOutput,
                             const std::wstring& seed,
                             RemainingIndex& remainingIndex,
                             unsigned int workerCount)
    {
        // --- Step 1: Extract patterns from restored filenames ---
        std::unordered_map<PatternGroupKey, NamePattern, PatternGroupKeyHash> patterns;
        std::wregex numberPattern(LR"(^(.+?)(\d+)([a-z]?)(\..*)$)");

        WIN32_FIND_DATAW pkgData{};
        HANDLE pkgFind = FindFirstFileW(Combine(restoredOutput, L"*").c_str(), &pkgData);
        if (pkgFind == INVALID_HANDLE_VALUE) return 0;

        do
        {
            if (IsDotEntry(pkgData.cFileName) || (pkgData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                continue;
            if (wcscmp(pkgData.cFileName, L"_fallback") == 0)
                continue;

            std::wstring pkgDir = Combine(restoredOutput, pkgData.cFileName);
            std::wstring pkgName = pkgData.cFileName;

            WIN32_FIND_DATAW dirData{};
            HANDLE dirFind = FindFirstFileW(Combine(pkgDir, L"*").c_str(), &dirData);
            if (dirFind == INVALID_HANDLE_VALUE) continue;

            do
            {
                if (IsDotEntry(dirData.cFileName) || (dirData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                    continue;

                std::wstring dirName = dirData.cFileName;
                std::wstring dirPath = Combine(pkgDir, dirName);

                WIN32_FIND_DATAW fileData{};
                HANDLE fileFind = FindFirstFileW(Combine(dirPath, L"*").c_str(), &fileData);
                if (fileFind == INVALID_HANDLE_VALUE) continue;

                do
                {
                    if (IsDotEntry(fileData.cFileName) || (fileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                        continue;

                    std::wstring fileName(fileData.cFileName);
                    std::wsmatch match;
                    if (std::regex_match(fileName, match, numberPattern))
                    {
                        std::wstring prefix = match[1].str();
                        std::wstring numStr = match[2].str();
                        std::wstring letter = match[3].str(); // e.g. "a" for bg001a.png
                        std::wstring suffix = match[4].str();
                        int number = _wtoi(numStr.c_str());

                        // Filter out hash-looking prefixes (fallback hash files with extensions)
                        // Hash names are pure uppercase hex (0-9, A-F); real names have other chars
                        bool isPureUpperHex = true;
                        for (wchar_t ch : prefix)
                        {
                            if (!((ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'F')))
                            { isPureUpperHex = false; break; }
                        }
                        if (isPureUpperHex) continue; // skip hash-named fallback files

                        // Include letter variant in pattern key
                        PatternGroupKey key{ pkgName, dirName, prefix, letter, suffix };
                        auto it = patterns.find(key);
                        if (it == patterns.end())
                        {
                            NamePattern np;
                            np.prefix = prefix;
                            np.letter = letter;
                            np.suffix = suffix;
                            np.numDigits = (int)numStr.size();
                            np.maxNumber = number;
                            np.numberStrs.push_back(numStr);
                            patterns[key] = np;
                        }
                        else
                        {
                            if (number > it->second.maxNumber) it->second.maxNumber = number;
                            if ((int)numStr.size() > it->second.numDigits) it->second.numDigits = (int)numStr.size();
                            // Keep a sample of original number strings for format preservation
                            if (it->second.numberStrs.size() < 5)
                                it->second.numberStrs.push_back(numStr);
                        }
                    }
                } while (FindNextFileW(fileFind, &fileData));
                FindClose(fileFind);
            } while (FindNextFileW(dirFind, &dirData));
            FindClose(dirFind);
        } while (FindNextFileW(pkgFind, &pkgData));
        FindClose(pkgFind);

        if (patterns.empty()) return 0;

        // --- Step 2: Collect pattern groups into task list ---
        struct PatternTask
        {
            PatternGroupKey key;
            NamePattern pattern;
            std::wstring dirHash; // precomputed dir hash
        };

        std::vector<PatternTask> tasks;
        tasks.reserve(patterns.size());
        for (const auto& entry : patterns)
        {
            PatternTask task;
            task.key = entry.first;
            task.pattern = entry.second;
            task.dirHash = StaticHashGeneratorLite::ComputeDirHash(entry.first.directoryName, seed);
            tasks.push_back(task);
        }

        // --- Step 3: Multi-threaded candidate generation + matching ---
        std::mutex resultMutex;
        std::atomic<size_t> nextTask{ 0 };
        std::atomic<int> totalRestored{ 0 };

        if (workerCount == 0) workerCount = 1;
        if (workerCount > tasks.size()) workerCount = (unsigned int)tasks.size();

        std::vector<std::thread> workers;
        workers.reserve(workerCount);

        for (unsigned int w = 0; w < workerCount; ++w)
        {
            workers.emplace_back([&]()
            {
                const int EXPAND_RANGE = 200;

                while (true)
                {
                    size_t taskIdx = nextTask.fetch_add(1);
                    if (taskIdx >= tasks.size()) break;

                    const PatternTask& task = tasks[taskIdx];
                    int startNum = task.pattern.maxNumber + 1;
                    int endNum = task.pattern.maxNumber + EXPAND_RANGE;

                    // Helper lambda: try a candidate, match against index, copy if found
                    auto tryCandidate = [&](const std::wstring& candidate) -> bool
                    {
                        std::wstring fileHash = StaticHashGeneratorLite::ComputeFileHash(candidate, seed);
                        std::lock_guard<std::mutex> lock(resultMutex);
                        auto it = remainingIndex.find(fileHash);
                        if (it != remainingIndex.end() && it->second.packageName == task.key.packageName)
                        {
                            std::wstring srcDir = Combine(Combine(extractorOutput, it->second.packageName), it->second.dirHash);
                            std::wstring srcPath = Combine(srcDir, NormalizeHash(fileHash));
                            EnsureDirectory(Combine(Combine(restoredOutput, task.key.packageName), task.key.directoryName));
                            std::wstring dstPath = Combine(Combine(restoredOutput, task.key.packageName), Combine(task.key.directoryName, candidate));
                            if (CopyFileW(srcPath.c_str(), dstPath.c_str(), FALSE))
                            {
                                ++totalRestored;
                            }
                            remainingIndex.erase(it);
                            return true;
                        }
                        return false;
                    };

                    // Rule A: Basic +200 expansion (max+1 to max+200)
                    for (int num = startNum; num <= endNum; ++num)
                    {
                        wchar_t numBuf[32]{};
                        swprintf_s(numBuf, L"%0*d", task.pattern.numDigits, num);
                        tryCandidate(task.pattern.prefix + numBuf + task.pattern.letter + task.pattern.suffix);
                    }

                    // Rule B: If suffix is .ogg, also try .sli and .ogg.sli using original format
                    if (task.pattern.suffix.find(L".ogg") != std::wstring::npos)
                    {
                        // Use original number strings (preserve underscore format)
                        for (const auto& origNum : task.pattern.numberStrs)
                        {
                            std::wstring base = task.pattern.prefix + origNum + task.pattern.letter;
                            tryCandidate(base + L".sli");
                            tryCandidate(base + L".ogg.sli");
                        }
                        // Fallback: reconstructed format
                        for (int num = 0; num <= task.pattern.maxNumber; ++num)
                        {
                            wchar_t numBuf[32]{};
                            swprintf_s(numBuf, L"%0*d", task.pattern.numDigits, num);
                            std::wstring base2 = task.pattern.prefix + numBuf + task.pattern.letter;
                            tryCandidate(base2 + L".sli");
                            tryCandidate(base2 + L".ogg.sli");
                        }
                    }

                    // Rule C: For small-number patterns (max <= 100), try full 0-999 range
                    if (task.pattern.maxNumber <= 100)
                    {
                        int maxDigits = task.pattern.numDigits > 3 ? task.pattern.numDigits : 3;
                        for (int num = 0; num <= 999; ++num)
                        {
                            wchar_t numBuf[32]{};
                            swprintf_s(numBuf, L"%0*d", maxDigits, num);
                            tryCandidate(task.pattern.prefix + numBuf + task.pattern.letter + task.pattern.suffix);
                        }
                    }

                    // Rule D: For patterns with letter variant, try all a-z letters
                    // (only if the pattern doesn't already have a letter)
                    {
                        // Check if any restored filename in this group has a letter variant
                        // by checking if the suffix in the pattern key contains letter + ext
                        // This is handled by the group key including the letter
                    }
                }
            });
        }

        for (auto& worker : workers) worker.join();

        return totalRestored.load();
    }

    // Phase 1.5: Hash Log Pattern Expansion
    // Uses the full FileNameHash.log as pattern source (richer patterns than restored files)
    int ExpandFromHashLog(const std::wstring& workspace,
                          const std::wstring& restoredOutput,
                          const std::wstring& extractorOutput,
                          const std::wstring& seed,
                          RemainingIndex& remainingIndex,
                          unsigned int workerCount)
    {
        std::wstring hashLogPath = Combine(Combine(workspace, L"StaticHash_Output"), L"FileNameHash.log");
        std::unordered_map<std::wstring, std::wstring> hashLogEntries;
        if (!LoadHashMap(hashLogPath, hashLogEntries))
        {
            return 0;
        }

        // Extract patterns from hash log
        struct HashPattern
        {
            std::wstring prefix;
            std::wstring suffix;
            int numDigits = 0;
            int maxNumber = -1;
            std::vector<std::wstring> numberStrs;
        };

        std::wregex numberPattern(LR"(^(.+?)(\d+)([a-z]?)(\..*)$)");
        std::unordered_map<std::wstring, HashPattern> hashPatterns;

        for (const auto& entry : hashLogEntries)
        {
            std::wstring name = entry.second;
            std::wsmatch match;
            if (std::regex_match(name, match, numberPattern))
            {
                std::wstring prefix = match[1].str();
                std::wstring numStr = match[2].str();
                std::wstring letter = match[3].str();
                std::wstring ext = match[4].str();
                // Skip hash-only prefixes
                bool isHex = true;
                for (wchar_t ch : prefix) {
                    if (!((ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'F'))) { isHex = false; break; }
                }
                if (isHex) continue;

                int num = _wtoi(numStr.c_str());
                std::wstring key = prefix + letter + ext; // unique key
                auto it = hashPatterns.find(key);
                if (it == hashPatterns.end())
                {
                    HashPattern hp;
                    hp.prefix = prefix + letter;
                    hp.suffix = ext;
                    hp.numDigits = (int)numStr.size();
                    hp.maxNumber = num;
                    hp.numberStrs.push_back(numStr);
                    hashPatterns[key] = hp;
                }
                else
                {
                    if (num > it->second.maxNumber) it->second.maxNumber = num;
                    if ((int)numStr.size() > it->second.numDigits) it->second.numDigits = (int)numStr.size();
                    if (it->second.numberStrs.size() < 5) it->second.numberStrs.push_back(numStr);
                }
            }
        }


        // Convert to vector for multi-threaded processing
        struct HashPatternTask
        {
            std::wstring prefix;
            std::wstring suffix;
            int numDigits;
            int maxNumber;
            std::vector<std::wstring> numberStrs; // original number strings from restored files
        };

        std::vector<HashPatternTask> tasks;
        tasks.reserve(hashPatterns.size());
        for (const auto& entry : hashPatterns)
        {
            tasks.push_back({ entry.second.prefix, entry.second.suffix, entry.second.numDigits, entry.second.maxNumber, entry.second.numberStrs });
        }

        std::mutex resultMutex;
        std::atomic<size_t> nextTask{ 0 };
        std::atomic<int> totalRestored{ 0 };

        if (workerCount == 0) workerCount = 1;
        if (workerCount > tasks.size()) workerCount = (unsigned int)tasks.size();

        std::vector<std::thread> workers;
        workers.reserve(workerCount);

        for (unsigned int w = 0; w < workerCount; ++w)
        {
            workers.emplace_back([&]()
            {
                while (true)
                {
                    size_t taskIdx = nextTask.fetch_add(1);
                    if (taskIdx >= tasks.size()) break;

                    const HashPatternTask& task = tasks[taskIdx];

                    auto tryCandidate = [&](const std::wstring& candidate) -> bool
                    {
                        std::wstring fileHash = StaticHashGeneratorLite::ComputeFileHash(candidate, seed);
                        std::lock_guard<std::mutex> lock(resultMutex);
                        auto it = remainingIndex.find(fileHash);
                        if (it != remainingIndex.end())
                        {
                            std::wstring srcDir = Combine(Combine(extractorOutput, it->second.packageName), it->second.dirHash);
                            std::wstring srcPath = Combine(srcDir, NormalizeHash(fileHash));
                            EnsureDirectory(restoredOutput);
                            // Put hash-log-inferred files in _inferred_hlog subdir per package
                            std::wstring inferDir = Combine(Combine(restoredOutput, it->second.packageName), L"_inferred_hlog");
                            EnsureDirectory(inferDir);
                            std::wstring dstPath = Combine(inferDir, candidate);
                            if (CopyFileW(srcPath.c_str(), dstPath.c_str(), FALSE))
                            {
                                ++totalRestored;
                            }
                            remainingIndex.erase(it);
                            return true;
                        }
                        return false;
                    };

                    // Rule A: Basic +200
                    for (int num = task.maxNumber + 1; num <= task.maxNumber + 200; ++num)
                    {
                        wchar_t buf[32]{};
                        swprintf_s(buf, L"%0*d", task.numDigits, num);
                        tryCandidate(task.prefix + buf + task.suffix);
                    }

                    // Rule B: .sli/.ogg.sli using original number format
                    if (task.suffix.find(L".ogg") != std::wstring::npos)
                    {
                        // Use original number strings (preserve underscore format)
                        for (const auto& origNum : task.numberStrs)
                        {
                            std::wstring base = task.prefix + origNum;
                            tryCandidate(base + L".sli");
                            tryCandidate(base + L".ogg.sli");
                        }
                        // Also try reconstructed format fallback
                        for (int num = 0; num <= task.maxNumber; ++num)
                        {
                            wchar_t buf[32]{};
                            swprintf_s(buf, L"%0*d", task.numDigits, num);
                            std::wstring base = task.prefix + buf;
                            tryCandidate(base + L".sli");
                            tryCandidate(base + L".ogg.sli");
                        }
                    }

                    // Rule C: Full 0-999 for small patterns
                    if (task.maxNumber <= 100)
                    {
                        int maxDigits = task.numDigits > 3 ? task.numDigits : 3;
                        for (int num = 0; num <= 999; ++num)
                        {
                            wchar_t buf[32]{};
                            swprintf_s(buf, L"%0*d", maxDigits, num);
                            tryCandidate(task.prefix + buf + task.suffix);
                        }
                    }
                }
            });
        }

        for (auto& worker : workers) worker.join();
        return totalRestored.load();
    }

    // Phase 1.5: .sli Inference from .ogg
    // For every restored .ogg file, infer its companion .ogg.sli by the same name + hash.
    // KrkrZ/Cxdec games store .sli files as "basename.ogg.sli" paired with "basename.ogg".
    int InferSliFromOgg(const std::wstring& restoredOutput,
                        const std::wstring& extractorOutput,
                        const std::wstring& seed,
                        RemainingIndex& remainingIndex,
                        unsigned int workerCount)
    {
        // Collect all restored .ogg files (non-hash names) from all package dirs
        struct SliTask
        {
            std::wstring packageName;
            std::wstring oggBaseName; // "anj_000_0001" from "anj_000_0001.ogg"
        };
        std::vector<SliTask> tasks;

        WIN32_FIND_DATAW pkgData{};
        HANDLE pkgFind = FindFirstFileW(Combine(restoredOutput, L"*").c_str(), &pkgData);
        if (pkgFind == INVALID_HANDLE_VALUE) return 0;
        do {
            if (IsDotEntry(pkgData.cFileName) || (pkgData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                continue;
            std::wstring pkgName = pkgData.cFileName;

            std::wstring pkgPath = Combine(restoredOutput, pkgName);
            WIN32_FIND_DATAW fileData{};
            HANDLE fileFind = FindFirstFileW(Combine(pkgPath, L"*").c_str(), &fileData);
            if (fileFind == INVALID_HANDLE_VALUE) continue;
            do {
                if (IsDotEntry(fileData.cFileName) || (fileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    continue;
                std::wstring fname = fileData.cFileName;
                // Match restored .ogg files (not hash-named, not .ogg.sli)
                if (fname.size() > 4 &&
                    fname.compare(fname.size() - 4, 4, L".ogg") == 0 &&
                    fname.find(L".ogg.sli") == std::wstring::npos &&
                    fname.find(L'_') != std::wstring::npos) // has underscore = real name
                {
                    std::wstring base = fname.substr(0, fname.size() - 4); // drop ".ogg"
                    tasks.push_back({ pkgName, base });
                }
            } while (FindNextFileW(fileFind, &fileData));
            FindClose(fileFind);
        } while (FindNextFileW(pkgFind, &pkgData));
        FindClose(pkgFind);

        if (tasks.empty())
        {
            return 0;
        }

        std::atomic<int> totalRestored(0);
        std::mutex copyMutex;

        auto workerFunc = [&](size_t startIdx, size_t endIdx) {
            for (size_t i = startIdx; i < endIdx; ++i)
            {
                const auto& task = tasks[i];
                std::wstring sliName = task.oggBaseName + L".ogg.sli";

                // Compute hash using existing engine
                std::wstring hash = StaticHashGeneratorLite::ComputeFileHash(sliName, seed);
                if (hash.empty()) continue;

                // Check if this hash is in the remaining (unrestored) index
                auto it = remainingIndex.find(hash);
                if (it == remainingIndex.end()) continue;

                // Copy from extractor output to restored output
                std::wstring srcDir = Combine(Combine(extractorOutput, it->second.packageName), it->second.dirHash);
                std::wstring srcPath = Combine(srcDir, NormalizeHash(hash));
                std::wstring dstDir = Combine(restoredOutput, task.packageName);
                EnsureDirectory(dstDir);
                std::wstring dstPath = Combine(dstDir, sliName);

                if (CopyFileW(srcPath.c_str(), dstPath.c_str(), FALSE))
                {
                    totalRestored.fetch_add(1);
                    {
                        std::lock_guard<std::mutex> lock(copyMutex);
                        remainingIndex.erase(it);
                    }
                }
            }
        };

        // Launch worker threads
        size_t totalTasks = tasks.size();
        size_t tasksPerWorker = (totalTasks + workerCount - 1) / workerCount;
        std::vector<std::thread> workers;
        for (unsigned int w = 0; w < workerCount; ++w)
        {
            size_t start = w * tasksPerWorker;
            size_t end = (std::min)(start + tasksPerWorker, totalTasks);
            if (start < end)
                workers.emplace_back(workerFunc, start, end);
        }
        for (auto& worker : workers) worker.join();

        return totalRestored.load();
    }

    // Phase 2: Reference Extraction
    // Scan restored .tjs/.ks/.csv files for quoted filename strings, hash them, match leftovers
    int ReferenceExtractRestore(const std::wstring& restoredOutput,
                                const std::wstring& extractorOutput,
                                const std::wstring& seed,
                                RemainingIndex& remainingIndex,
                                unsigned int workerCount)
    {
        // --- Step 1: Collect source files (.tjs, .ks, .csv) from restored output ---
        struct SourceFileTask
        {
            std::wstring packageName;
            std::wstring filePath;
        };

        std::vector<SourceFileTask> sourceFiles;

        WIN32_FIND_DATAW pkgData{};
        HANDLE pkgFind = FindFirstFileW(Combine(restoredOutput, L"*").c_str(), &pkgData);
        if (pkgFind == INVALID_HANDLE_VALUE) return 0;

        do
        {
            if (IsDotEntry(pkgData.cFileName) || (pkgData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                continue;
            if (wcscmp(pkgData.cFileName, L"_fallback") == 0)
                continue;

            std::wstring pkgDir = Combine(restoredOutput, pkgData.cFileName);
            std::wstring pkgName = pkgData.cFileName;

            // Recursively scan for .tjs, .ks, .csv files
            std::vector<std::wstring> dirStack;
            dirStack.push_back(pkgDir);

            while (!dirStack.empty())
            {
                std::wstring currentDir = dirStack.back();
                dirStack.pop_back();

                WIN32_FIND_DATAW findData{};
                HANDLE findHandle = FindFirstFileW(Combine(currentDir, L"*").c_str(), &findData);
                if (findHandle == INVALID_HANDLE_VALUE) continue;

                do
                {
                    if (IsDotEntry(findData.cFileName)) continue;

                    std::wstring childPath = Combine(currentDir, findData.cFileName);

                    if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    {
                        dirStack.push_back(childPath);
                    }
                    else
                    {
                        // Check extension
                        const wchar_t* ext = wcsrchr(findData.cFileName, L'.');
                        if (ext)
                        {
                            if (_wcsicmp(ext, L".tjs") == 0 || _wcsicmp(ext, L".ks") == 0 || _wcsicmp(ext, L".csv") == 0)
                            {
                                sourceFiles.push_back({ pkgName, childPath });
                            }
                        }
                    }
                } while (FindNextFileW(findHandle, &findData));
                FindClose(findHandle);
            }
        } while (FindNextFileW(pkgFind, &pkgData));
        FindClose(pkgFind);

        if (sourceFiles.empty()) return 0;

        // --- Step 2: Multi-threaded reference extraction + matching ---
        std::mutex resultMutex;
        std::atomic<size_t> nextFile{ 0 };
        std::atomic<int> totalRestored{ 0 };

        if (workerCount == 0) workerCount = 1;
        if (workerCount > sourceFiles.size()) workerCount = (unsigned int)sourceFiles.size();

        std::vector<std::thread> workers;
        workers.reserve(workerCount);

        for (unsigned int w = 0; w < workerCount; ++w)
        {
            workers.emplace_back([&]()
            {
                // Each worker maintains its own set of already-seen candidates per package
                // to avoid redundant hashing within the same worker
                std::unordered_map<std::wstring, std::set<std::wstring>> seenCandidates;

                while (true)
                {
                    size_t idx = nextFile.fetch_add(1);
                    if (idx >= sourceFiles.size()) break;

                    const SourceFileTask& task = sourceFiles[idx];

                    // Read file content with text/binary fallback
                    std::vector<std::wstring> extractedStrings;
                    if (!ReadTextFileForStrings(task.filePath, extractedStrings)) continue;

                    // Process each extracted string
                    auto& seen = seenCandidates[task.packageName];
                    for (const std::wstring& quoted : extractedStrings)
                    {
                        // Filter candidates (extra safety)
                        if (quoted.size() < 4 || quoted.size() > 260) continue;
                        if (quoted.find(L'/') != std::wstring::npos) continue;
                        if (quoted.find(L'\\') != std::wstring::npos) continue;
                        if (quoted.find_first_of(L"<>:\"|?*") != std::wstring::npos) continue;
                        if (seen.find(quoted) != seen.end()) continue;

                        seen.insert(quoted);

                        // Compute file hash and check against remaining index
                        std::wstring fileHash = StaticHashGeneratorLite::ComputeFileHash(quoted, seed);

                        std::lock_guard<std::mutex> lock(resultMutex);
                        auto it2 = remainingIndex.find(fileHash);
                        if (it2 != remainingIndex.end() && it2->second.packageName == task.packageName)
                        {
                            // Found match! Determine target directory
                            std::wstring srcDir = Combine(Combine(extractorOutput, it2->second.packageName), it2->second.dirHash);
                            std::wstring srcPath = Combine(srcDir, NormalizeHash(fileHash));

                            // We don't know the original directory for reference-extracted files
                            // Place them in a dedicated _inferred subdirectory for manual review
                            std::wstring inferDir = Combine(Combine(restoredOutput, task.packageName), L"_inferred");
                            EnsureDirectory(inferDir);
                            std::wstring dstPath = Combine(inferDir, quoted);

                            if (CopyFileW(srcPath.c_str(), dstPath.c_str(), FALSE))
                            {
                                ++totalRestored;
                            }

                            remainingIndex.erase(it2);
                        }
                    }
                }
            });
        }

        for (auto& worker : workers) worker.join();

        return totalRestored.load();
    }

    // Count real-named files per directory (correct hash detection: real names have lowercase letters)
    std::unordered_map<std::wstring, unsigned int> CountPerDirRealNames(const std::wstring& restoredOutput)
    {
        std::unordered_map<std::wstring, unsigned int> result;
        WIN32_FIND_DATAW pkgData{};
        HANDLE pkgFind = FindFirstFileW(Combine(restoredOutput, L"*").c_str(), &pkgData);
        if (pkgFind == INVALID_HANDLE_VALUE) return result;

        do {
            if (IsDotEntry(pkgData.cFileName) || (pkgData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
            if (pkgData.cFileName[0] == L'_') continue;

            std::wstring pkgDir = Combine(restoredOutput, pkgData.cFileName);
            std::wstring pkgName = pkgData.cFileName;
            unsigned int count = 0;

            // Recursively scan all files in this package directory (including subdirs)
            std::vector<std::wstring> dirStack;
            dirStack.push_back(pkgDir);

            while (!dirStack.empty()) {
                std::wstring currentDir = dirStack.back();
                dirStack.pop_back();

                WIN32_FIND_DATAW findData{};
                HANDLE findHandle = FindFirstFileW(Combine(currentDir, L"*").c_str(), &findData);
                if (findHandle == INVALID_HANDLE_VALUE) continue;

                do {
                    if (IsDotEntry(findData.cFileName)) continue;
                    std::wstring childPath = Combine(currentDir, findData.cFileName);

                    if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                        if (findData.cFileName[0] != L'_') // skip _inferred, _inferred_hlog
                            dirStack.push_back(childPath);
                    } else {
                        std::wstring name(findData.cFileName);
                        size_t dot = name.find_last_of(L'.');
                        std::wstring stem = (dot != std::wstring::npos) ? name.substr(0, dot) : name;
                        bool hasLower = false;
                        for (wchar_t c : stem) {
                            if (c >= L'a' && c <= L'z') { hasLower = true; break; }
                        }
                        if (hasLower) ++count;
                    }
                } while (FindNextFileW(findHandle, &findData));
                FindClose(findHandle);
            }

            result[pkgName] = count;
        } while (FindNextFileW(pkgFind, &pkgData));
        FindClose(pkgFind);
        return result;
    }

    // Update restore report with per-directory inference breakdown
    void AppendInferenceToReport(const std::wstring& reportPath, int inferenceRestored,
                                  const std::unordered_map<std::wstring, unsigned int>& beforeDirs,
                                  const std::unordered_map<std::wstring, unsigned int>& afterDirs)
    {
        if (inferenceRestored <= 0) return;

        std::wstring content;
        if (!ReadUtf16File(reportPath, content)) return;

        unsigned int totalFiles = 0, originalRestored = 0;
        auto parseLine = [&](const std::wstring& prefix) -> unsigned int {
            size_t pos = content.find(prefix);
            if (pos == std::wstring::npos) return 0;
            pos += prefix.size();
            while (pos < content.size() && content[pos] == L' ') ++pos;
            size_t end = content.find_first_of(L"\r\n", pos);
            return (unsigned int)_wtoi(content.substr(pos, end - pos).c_str());
        };
        totalFiles = parseLine(L"总文件数:");
        originalRestored = parseLine(L"成功还原:");

        // 用磁盘扫描的前后差值计算推理实际新增的文件数
        // CountPerDirRealNames 统计文件名主干含小写字母的文件（排除纯Hash命名的fallback）
        unsigned int perDirBefore = 0, perDirAfter = 0;
        for (const auto& a : beforeDirs) perDirBefore += a.second;
        for (const auto& a : afterDirs) perDirAfter += a.second;
        int realInferenceGain = (int)perDirAfter - (int)perDirBefore;
        if (realInferenceGain < 0) realInferenceGain = 0;
        if (realInferenceGain > inferenceRestored) realInferenceGain = inferenceRestored;
        unsigned int finalRestored = originalRestored + (unsigned int)realInferenceGain;
        if (finalRestored > totalFiles) finalRestored = totalFiles;
        std::wstring finalRate = FormatPercent(finalRestored, totalFiles);

        // Update per-directory lines with inference gains
        std::wstring newContent;
        size_t dirSection = content.find(L"--- 各目录还原情况 ---");
        if (dirSection != std::wstring::npos) {
            newContent = content.substr(0, dirSection);
            newContent += L"--- 各目录还原情况 ---\r\n";
            size_t lineStart = content.find(L"\r\n", dirSection);
            if (lineStart != std::wstring::npos) lineStart += 2;

            while (lineStart < content.size()) {
                size_t lineEnd = content.find(L"\r\n", lineStart);
                std::wstring line;
                if (lineEnd == std::wstring::npos) { line = content.substr(lineStart); lineStart = content.size(); }
                else { line = content.substr(lineStart, lineEnd - lineStart); lineStart = lineEnd + 2; }

                if (line.empty() || line.find(L'│') == std::wstring::npos) break;

                size_t firstPipe = line.find(L'│');
                std::wstring dirName = line.substr(0, firstPipe);
                size_t slash = line.find(L'/', firstPipe + 1);
                if (slash == std::wstring::npos) { newContent += line + L"\r\n"; continue; }

                unsigned int old = (unsigned int)_wtoi(line.substr(firstPipe + 1, slash - firstPipe - 1).c_str());
                unsigned int total = (unsigned int)_wtoi(line.substr(slash + 1).c_str());
                unsigned int after = 0;
                auto it = afterDirs.find(dirName);
                if (it != afterDirs.end()) after = it->second;
                unsigned int gain = (after > old) ? (after - old) : 0;

                // If inference found files, use updated count; otherwise keep original
                unsigned int displayCount = (gain > 0) ? after : old;
                newContent += dirName + L"│" + std::to_wstring(displayCount) + L"/" + std::to_wstring(total)
                           + L"│" + FormatPercent(displayCount, total) + L"\r\n";
            }
        }

        // Add inference breakdown and final summary
        newContent += L"\r\n--- 推理补充还原 ---\r\n";
        newContent += L"模式扩展: ";
        bool first = true;
        int totalGain = 0;
        for (const auto& a : afterDirs) {
            auto b = beforeDirs.find(a.first);
            if (b != beforeDirs.end()) {
                int gain = (int)a.second - (int)b->second;
                if (gain > 0) {
                    if (!first) newContent += L" | ";
                    newContent += a.first + L" +" + std::to_wstring(gain);
                    first = false;
                    totalGain += gain;
                }
            }
        }
        newContent += L"\r\n";
        newContent += L"推理合计: +" + std::to_wstring(totalGain) + L"\r\n";
        newContent += L"\r\n=== 最终结果 ===\r\n";
        // 最终还原数 = 初始还原数 + 推理补充数
        // 不使用 CountPerDirRealNames 的磁盘扫描结果代替，因为该函数用"文件名含小写字母"
        // 来推断是否已还原，全大写文件名（如 CG001、TITLE、BGM_01）会被漏计，导致数值偏低。
        if (finalRestored > totalFiles) finalRestored = totalFiles;
        finalRate = FormatPercent(finalRestored, totalFiles);
        newContent += L"最终还原: " + std::to_wstring(finalRestored) + L" / " + std::to_wstring(totalFiles) + L"\r\n";
        newContent += L"最终成功率: " + finalRate + L"\r\n";
        unsigned int remaining = totalFiles - finalRestored;
        newContent += L"剩余未还原: " + std::to_wstring(remaining) + L"\r\n";
        WriteUtf16File(reportPath, newContent);
    }
}

bool ResourceRestorerLite::RestoreWorkspace(const std::wstring& workspace,
                                            Result& result,
                                            std::wstring& errorMessage)
{
    return RestoreWorkspaceEx(workspace, 1u, false, result, errorMessage, nullptr, nullptr);
}

bool ResourceRestorerLite::RestoreWorkspaceEx(const std::wstring& workspace,
                                              unsigned int workerCount,
                                              bool enableFallback,
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
    std::map<std::wstring, PackageStats> packageResults;
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

                PackageStats packageResult{};
                RestorePackage(tasks[index], restoredOutput, directoryMap, fileNameMap, packageResult, enableFallback, progressCallback, progressContext);

                std::lock_guard<std::mutex> lock(resultLock);
                AddPackageStats(packageResult, result);
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
    SYSTEMTIME reportTime{};
    ::GetLocalTime(&reportTime);
    wchar_t timeBuf[64]{};
    swprintf_s(timeBuf, L"%04u-%02u-%02u %02u:%02u:%02u", reportTime.wYear, reportTime.wMonth, reportTime.wDay, reportTime.wHour, reportTime.wMinute, reportTime.wSecond);
    report += L"资源文件名还原报告\r\n";
    report += std::wstring(L"还原时间: ") + timeBuf + L"\r\n\r\n";
    report += L"工作区: " + workspace + L"\r\n";
    report += L"工作线程: " + std::to_wstring(workerCount) + L"\r\n";
    report += L"总文件数: " + std::to_wstring(result.totalFiles) + L"\r\n";
    report += L"成功还原: " + std::to_wstring(result.restoredFiles) + L"\r\n";
    report += L"成功率: " + FormatPercent(result.restoredFiles, result.totalFiles) + L"\r\n";
    report += L"缺少目录 Hash: " + std::to_wstring(result.missingDirectoryHash) + L"\r\n";
    report += L"缺少文件名 Hash: " + std::to_wstring(result.missingFileNameHash) + L"\r\n";
    report += L"复制失败: " + std::to_wstring(result.copyFailed) + L"\r\n";
    if (result.fallbackRestoredFiles > 0)
        report += L"按后缀名保留（未匹配到文件名）: " + std::to_wstring(result.fallbackRestoredFiles) + L"\r\n";
    report += L"\r\n--- 各目录还原情况 ---\r\n";
    result.packages.clear();
    result.packages.reserve(packageResults.size());
    for (const auto& entry : packageResults)
    {
        PackageResult pkg{};
        pkg.packageName = entry.first;
        pkg.totalFiles = entry.second.totalFiles;
        pkg.restoredFiles = entry.second.restoredFiles;
        result.packages.push_back(pkg);

        report += entry.first + L"│" + std::to_wstring(entry.second.restoredFiles)
               + L"/" + std::to_wstring(entry.second.totalFiles)
               + L"│" + FormatPercent(entry.second.restoredFiles, entry.second.totalFiles) + L"\r\n";
    }
    WriteUtf16File(result.reportPath, report);
    return true;
}


// Secondary inference - analyzes restored filenames to infer naming patterns
// and recover additional hash-named files. Returns count of newly restored files.
int ResourceRestorerLite::SecondaryInference(const std::wstring& workspace, unsigned int workerCount)
{
    std::wstring seed = DetectHashSeed(workspace);
    if (seed.empty()) {
        seed = DetectHashSeed(DirectoryName(workspace));
        if (seed.empty()) seed = L"xp3hnp";
    }

    std::wstring restoredOutput = Combine(workspace, L"Restored_Extractor_Output");
    std::wstring extractorOutput = Combine(workspace, L"Extractor_Output");
    if (!DirectoryExists(restoredOutput) || !DirectoryExists(extractorOutput)) return 0;

    RemainingIndex remainingIndex = BuildRemainingIndex(extractorOutput);
    // Filter out entries already matched by static hash
    {
        std::wstring hashLogPath = Combine(Combine(workspace, L"StaticHash_Output"), L"FileNameHash.log");
        std::unordered_map<std::wstring, std::wstring> logMap;
        if (LoadHashMap(hashLogPath, logMap)) {
            for (const auto& entry : logMap)
                remainingIndex.erase(entry.first);
        }
    }
    // Also filter out entries matched by dynamic hash (if present)
    {
        std::wstring dynHashPath = Combine(Combine(workspace, L"StringHashDumper_Output"), L"FileNameHash.log");
        std::unordered_map<std::wstring, std::wstring> dynMap;
        if (LoadHashMap(dynHashPath, dynMap)) {
            for (const auto& entry : dynMap)
                remainingIndex.erase(entry.first);
        }
    }

    if (remainingIndex.empty()) return 0;

    // Record per-directory counts BEFORE inference
    std::unordered_map<std::wstring, unsigned int> beforeDirs = CountPerDirRealNames(restoredOutput);

    int totalRestored = 0;
    totalRestored += PatternExpandRestore(restoredOutput, extractorOutput, seed, remainingIndex, workerCount);
    if (!remainingIndex.empty())
        totalRestored += ExpandFromHashLog(workspace, restoredOutput, extractorOutput, seed, remainingIndex, workerCount);
    if (!remainingIndex.empty())
        totalRestored += ReferenceExtractRestore(restoredOutput, extractorOutput, seed, remainingIndex, workerCount);
    if (!remainingIndex.empty())
        totalRestored += InferSliFromOgg(restoredOutput, extractorOutput, seed, remainingIndex, workerCount);

    if (totalRestored <= 0) return 0;

    // Record per-directory counts AFTER inference
    std::unordered_map<std::wstring, unsigned int> afterDirs = CountPerDirRealNames(restoredOutput);

    std::wstring reportPath = Combine(restoredOutput, L"RestoreReport.txt");
    AppendInferenceToReport(reportPath, totalRestored, beforeDirs, afterDirs);

    // 用磁盘前后差值计算推理实际新增数（去重后的真实值），返回给 UI 弹窗显示
    int realGain = 0;
    for (const auto& a : afterDirs) realGain += (int)a.second;
    for (const auto& a : beforeDirs) realGain -= (int)a.second;
    if (realGain < 0) realGain = 0;
    if (realGain > totalRestored) realGain = totalRestored;

    // Clean up _inferred and _inferred_hlog temp directories
    auto RecursiveDelete = [](const std::wstring& rootDir) {
        // Collect all dirs bottom-up using a stack
        std::vector<std::wstring> allDirs;
        std::vector<std::wstring> stack;
        stack.push_back(rootDir);
        while (!stack.empty()) {
            std::wstring cur = stack.back(); stack.pop_back();
            allDirs.push_back(cur);
            WIN32_FIND_DATAW fd{};
            HANDLE fh = FindFirstFileW(Combine(cur, L"*").c_str(), &fd);
            if (fh != INVALID_HANDLE_VALUE) {
                do {
                    if (!IsDotEntry(fd.cFileName) && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                        stack.push_back(Combine(cur, fd.cFileName));
                    else if (!IsDotEntry(fd.cFileName))
                        DeleteFileW(Combine(cur, fd.cFileName).c_str());
                } while (FindNextFileW(fh, &fd));
                FindClose(fh);
            }
        }
        // Remove dirs bottom-up
        for (auto it = allDirs.rbegin(); it != allDirs.rend(); ++it)
            RemoveDirectoryW(it->c_str());
    };

    WIN32_FIND_DATAW pkgFd{};
    HANDLE pkgFh = FindFirstFileW(Combine(restoredOutput, L"*").c_str(), &pkgFd);
    if (pkgFh != INVALID_HANDLE_VALUE) {
        do {
            if (!IsDotEntry(pkgFd.cFileName) && (pkgFd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                std::wstring inferDir = Combine(Combine(restoredOutput, pkgFd.cFileName), L"_inferred");
                if (DirectoryExists(inferDir)) RecursiveDelete(inferDir);
                std::wstring hlogDir = Combine(Combine(restoredOutput, pkgFd.cFileName), L"_inferred_hlog");
                if (DirectoryExists(hlogDir)) RecursiveDelete(hlogDir);
            }
        } while (FindNextFileW(pkgFh, &pkgFd));
        FindClose(pkgFh);
    }

    return realGain;
}
