#include "ExtractorOutputRestorer.h"

#include <windows.h>

#include <cwchar>
#include <unordered_map>
#include <vector>

#include "directory.h"
#include "file.h"
#include "path.h"

namespace
{
    constexpr const wchar_t ExtractorOutputFolder[] = L"Extractor_Output";
    constexpr const wchar_t StringHashOutputFolder[] = L"StringHashDumper_Output";
    constexpr const wchar_t StaticHashOutputFolder[] = L"StaticHash_Output";
    constexpr const wchar_t RestoredOutputFolder[] = L"Restored_Extractor_Output";
    constexpr const wchar_t DirectoryHashFileName[] = L"DirectoryHash.log";
    constexpr const wchar_t FileNameHashFileName[] = L"FileNameHash.log";
    constexpr const wchar_t RestoreReportFileName[] = L"RestoreReport.txt";
    constexpr const wchar_t EmptyDirectoryMarker[] = L"%EmptyString%";
    constexpr const wchar_t HashLogSplit[] = L"##YSig##";

    bool IsDotEntry(const wchar_t* name)
    {
        return wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0;
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
        output.reserve(hash.length());

        for (wchar_t ch : hash)
        {
            if (ch >= L'a' && ch <= L'f')
            {
                output.push_back(ch - L'a' + L'A');
            }
            else if ((ch >= L'A' && ch <= L'F') || (ch >= L'0' && ch <= L'9'))
            {
                output.push_back(ch);
            }
        }

        return output;
    }

    bool IsUnsafeRestoredPath(const std::wstring& path)
    {
        if (path.empty())
        {
            return false;
        }

        if (path.length() >= 2 && path[1] == L':')
        {
            return true;
        }

        if (path.length() >= 2 && path[0] == L'\\' && path[1] == L'\\')
        {
            return true;
        }

        size_t start = 0;
        while (start <= path.length())
        {
            size_t end = path.find_first_of(L"\\/", start);
            std::wstring part = path.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
            if (part == L"..")
            {
                return true;
            }

            if (end == std::wstring::npos)
            {
                break;
            }
            start = end + 1;
        }

        return false;
    }

    bool ReadUtf16File(const std::wstring& path, std::wstring& output)
    {
        HANDLE file = ::CreateFileW(path.c_str(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 0x7ffffffe)
        {
            ::CloseHandle(file);
            return false;
        }

        std::vector<unsigned char> buffer((size_t)size.QuadPart);
        DWORD read = 0;
        bool success = ::ReadFile(file, buffer.data(), (DWORD)buffer.size(), &read, nullptr) && read == buffer.size();
        ::CloseHandle(file);

        if (!success || buffer.size() < sizeof(wchar_t))
        {
            return false;
        }

        size_t offset = 0;
        if (buffer.size() >= 2 && buffer[0] == 0xff && buffer[1] == 0xfe)
        {
            offset = 2;
        }

        size_t wcharCount = (buffer.size() - offset) / sizeof(wchar_t);
        output.assign((const wchar_t*)(buffer.data() + offset), wcharCount);
        return true;
    }

    bool LoadHashMap(const std::wstring& path, std::unordered_map<std::wstring, std::wstring>& output)
    {
        std::wstring content;
        if (!ReadUtf16File(path, content))
        {
            return false;
        }

        size_t start = 0;
        while (start < content.length())
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
                    if (text == EmptyDirectoryMarker)
                    {
                        text.clear();
                    }
                    output[hash] = text;
                }
            }

            if (end == std::wstring::npos)
            {
                break;
            }
            start = end + 1;
        }

        return true;
    }

    void AddDirectoryResult(ExtractorOutputRestorer::Result& result, const ExtractorOutputRestorer::DirectoryResult& directory)
    {
        result.TotalFiles += directory.TotalFiles;
        result.RestoredFiles += directory.RestoredFiles;
        result.MissingDirectoryHash += directory.MissingDirectoryHash;
        result.MissingFileNameHash += directory.MissingFileNameHash;
        result.CopyFailed += directory.CopyFailed;
        result.Directories.push_back(directory);
    }

    void RestorePackage(const std::wstring& packageDirectory,
                        const std::wstring& packageName,
                        const std::wstring& outputRoot,
                        const std::unordered_map<std::wstring, std::wstring>& directoryMap,
                        const std::unordered_map<std::wstring, std::wstring>& fileNameMap,
                        ExtractorOutputRestorer::DirectoryResult& result)
    {
        result.Name = packageName;

        std::wstring searchPattern = Path::Combine(packageDirectory, L"*");
        WIN32_FIND_DATAW directoryData{};
        HANDLE directoryFind = ::FindFirstFileW(searchPattern.c_str(), &directoryData);
        if (directoryFind == INVALID_HANDLE_VALUE)
        {
            return;
        }

        do
        {
            if (IsDotEntry(directoryData.cFileName) || (directoryData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                continue;
            }

            std::wstring directoryHash = NormalizeHash(directoryData.cFileName);
            std::wstring hashDirectory = Path::Combine(packageDirectory, directoryData.cFileName);
            std::wstring fileSearchPattern = Path::Combine(hashDirectory, L"*");
            WIN32_FIND_DATAW fileData{};
            HANDLE fileFind = ::FindFirstFileW(fileSearchPattern.c_str(), &fileData);
            if (fileFind == INVALID_HANDLE_VALUE)
            {
                continue;
            }

            auto directoryIt = directoryMap.find(directoryHash);

            do
            {
                if (IsDotEntry(fileData.cFileName) || (fileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                {
                    continue;
                }

                ++result.TotalFiles;

                if (directoryIt == directoryMap.end())
                {
                    ++result.MissingDirectoryHash;
                    continue;
                }

                std::wstring fileHash = NormalizeHash(fileData.cFileName);
                auto fileNameIt = fileNameMap.find(fileHash);
                if (fileNameIt == fileNameMap.end())
                {
                    ++result.MissingFileNameHash;
                    continue;
                }

                std::wstring restoredDirectory = directoryIt->second;
                std::wstring restoredFileName = fileNameIt->second;
                if (IsUnsafeRestoredPath(restoredFileName))
                {
                    ++result.MissingFileNameHash;
                    continue;
                }

                std::wstring packageOutput = Path::Combine(outputRoot, packageName);
                std::wstring outputDirectory = restoredDirectory.empty() ? packageOutput : Path::Combine(packageOutput, restoredDirectory);
                Directory::Create(outputDirectory);

                std::wstring sourcePath = Path::Combine(hashDirectory, fileData.cFileName);
                std::wstring targetPath = Path::Combine(outputDirectory, restoredFileName);
                if (!::CopyFileW(sourcePath.c_str(), targetPath.c_str(), FALSE))
                {
                    ++result.CopyFailed;
                    continue;
                }

                ++result.RestoredFiles;
            } while (::FindNextFileW(fileFind, &fileData));

            ::FindClose(fileFind);
        } while (::FindNextFileW(directoryFind, &directoryData));

        ::FindClose(directoryFind);
    }

    std::wstring FormatPercent(unsigned int value, unsigned int total)
    {
        if (total == 0)
        {
            return L"0.00%";
        }

        unsigned int scaled = (unsigned int)(((unsigned long long)value * 10000ull + total / 2u) / total);
        wchar_t text[32]{};
        wsprintfW(text, L"%u.%02u%%", scaled / 100u, scaled % 100u);
        return text;
    }

    void WriteRestoreReport(ExtractorOutputRestorer::Result& result, const std::wstring& restoredOutput)
    {
        result.ReportPath = Path::Combine(restoredOutput, RestoreReportFileName);

        std::wstring report;
        report += L"资源文件名还原报告\r\n";
        report += L"\r\n";
        report += L"输出目录：Restored_Extractor_Output\r\n";
        report += L"总文件数：" + std::to_wstring(result.TotalFiles) + L"\r\n";
        report += L"成功还原：" + std::to_wstring(result.RestoredFiles) + L"\r\n";
        report += L"成功率：" + FormatPercent(result.RestoredFiles, result.TotalFiles) + L"\r\n";
        report += L"缺少目录Hash：" + std::to_wstring(result.MissingDirectoryHash) + L"\r\n";
        report += L"缺少文件名Hash：" + std::to_wstring(result.MissingFileNameHash) + L"\r\n";
        report += L"复制失败：" + std::to_wstring(result.CopyFailed) + L"\r\n";
        report += L"\r\n";
        report += L"各目录还原情况：\r\n";

        for (const auto& directory : result.Directories)
        {
            report += L"\r\n";
            report += L"目录：" + directory.Name + L"\r\n";
            report += L"  总文件数：" + std::to_wstring(directory.TotalFiles) + L"\r\n";
            report += L"  成功还原：" + std::to_wstring(directory.RestoredFiles) + L"\r\n";
            report += L"  成功率：" + FormatPercent(directory.RestoredFiles, directory.TotalFiles) + L"\r\n";
            report += L"  缺少目录Hash：" + std::to_wstring(directory.MissingDirectoryHash) + L"\r\n";
            report += L"  缺少文件名Hash：" + std::to_wstring(directory.MissingFileNameHash) + L"\r\n";
            report += L"  复制失败：" + std::to_wstring(directory.CopyFailed) + L"\r\n";
        }

        WORD bom = 0xfeff;
        std::vector<unsigned char> buffer(sizeof(bom) + report.length() * sizeof(wchar_t));
        memcpy(buffer.data(), &bom, sizeof(bom));
        memcpy(buffer.data() + sizeof(bom), report.data(), report.length() * sizeof(wchar_t));
        File::WriteAllBytes(result.ReportPath, buffer.data(), buffer.size());
    }
}

ExtractorOutputRestorer::ExtractorOutputRestorer(const std::wstring& gameDirectory)
    : mGameDirectory(gameDirectory)
{
}

bool ExtractorOutputRestorer::Restore(Result& result, std::wstring& errorMessage)
{
    result = Result{};
    errorMessage.clear();

    std::wstring extractorOutput = Path::Combine(this->mGameDirectory, ExtractorOutputFolder);
    std::wstring hashOutput = Path::Combine(this->mGameDirectory, StringHashOutputFolder);
    std::wstring staticHashOutput = Path::Combine(this->mGameDirectory, StaticHashOutputFolder);
    std::wstring restoredOutput = Path::Combine(this->mGameDirectory, RestoredOutputFolder);

    if (!Directory::Exists(extractorOutput))
    {
        errorMessage = L"未找到 Extractor_Output 目录。";
        return false;
    }

    if (!Directory::Exists(hashOutput) && !Directory::Exists(staticHashOutput))
    {
        errorMessage = L"未找到 StringHashDumper_Output 目录。";
        return false;
    }

    std::unordered_map<std::wstring, std::wstring> directoryMap;
    std::unordered_map<std::wstring, std::wstring> fileNameMap;

    if (Directory::Exists(hashOutput) && !LoadHashMap(Path::Combine(hashOutput, DirectoryHashFileName), directoryMap))
    {
        errorMessage = L"读取 DirectoryHash.log 失败。";
        return false;
    }

    if (Directory::Exists(hashOutput) && !LoadHashMap(Path::Combine(hashOutput, FileNameHashFileName), fileNameMap))
    {
        errorMessage = L"读取 FileNameHash.log 失败。";
        return false;
    }

    if (Directory::Exists(staticHashOutput))
    {
        LoadHashMap(Path::Combine(staticHashOutput, DirectoryHashFileName), directoryMap);
        LoadHashMap(Path::Combine(staticHashOutput, FileNameHashFileName), fileNameMap);
    }

    if (directoryMap.empty() || fileNameMap.empty())
    {
        errorMessage = L"Hash 日志为空，无法还原资源文件名。";
        return false;
    }

    Directory::Create(restoredOutput);

    std::wstring packageSearchPattern = Path::Combine(extractorOutput, L"*");
    WIN32_FIND_DATAW packageData{};
    HANDLE packageFind = ::FindFirstFileW(packageSearchPattern.c_str(), &packageData);
    if (packageFind == INVALID_HANDLE_VALUE)
    {
        errorMessage = L"Extractor_Output 中没有可处理的目录。";
        return false;
    }

    do
    {
        if (IsDotEntry(packageData.cFileName) || (packageData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            continue;
        }

        DirectoryResult directoryResult{};
        RestorePackage(Path::Combine(extractorOutput, packageData.cFileName),
                       packageData.cFileName,
                       restoredOutput,
                       directoryMap,
                       fileNameMap,
                       directoryResult);
        AddDirectoryResult(result, directoryResult);
    } while (::FindNextFileW(packageFind, &packageData));

    ::FindClose(packageFind);

    if (result.TotalFiles == 0)
    {
        errorMessage = L"Extractor_Output 中没有找到 hash 文件。";
        return false;
    }

    if (result.RestoredFiles == 0)
    {
        errorMessage = L"没有文件被还原，请确认 hash 日志与解包结果来自同一个游戏。";
        return false;
    }

    WriteRestoreReport(result, restoredOutput);

    return true;
}
