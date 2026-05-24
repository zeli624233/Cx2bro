#include "PublisherExtensionBuilderLite.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
    struct VoicePatternCandidate
    {
        std::wstring root;
        std::wstring extension;
        unsigned int numberWidth = 4;
        unsigned int maxNumber = 0;
        std::set<std::wstring> prefixes;
        std::set<std::wstring> suffixes;
        std::map<std::wstring, unsigned int> prefixMaxNumbers;
        unsigned int matchCount = 0;
    };

    struct RestoreStats
    {
        unsigned int totalFiles = 0;
        unsigned int restoredFiles = 0;
        bool hasStats = false;
    };

    struct GenericPatternCandidate
    {
        std::wstring pattern;
        unsigned int numberWidth = 0;
        unsigned int maxNumber = 0;
        unsigned int numberWidth2 = 0;
        unsigned int maxNumber2 = 0;
        bool hasSecondNumber = false;
        std::set<std::wstring> suffixes;
        std::set<std::wstring> coveredPaths;
        unsigned int generatedCount = 0;
    };

    std::wstring Combine(const std::wstring& left, const std::wstring& right)
    {
        if (left.empty()) return right;
        if (right.empty()) return left;
        if (left.back() == L'\\' || left.back() == L'/') return left + right;
        return left + L"\\" + right;
    }

    std::wstring DirectoryName(const std::wstring& path)
    {
        size_t pos = path.find_last_of(L"\\/");
        return pos == std::wstring::npos ? L"" : path.substr(0, pos);
    }

    std::wstring FileStem(const std::wstring& path)
    {
        size_t slash = path.find_last_of(L"\\/");
        size_t start = slash == std::wstring::npos ? 0 : slash + 1;
        size_t dot = path.find_last_of(L'.');
        if (dot == std::wstring::npos || dot < start)
        {
            dot = path.size();
        }
        return path.substr(start, dot - start);
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

    void EnsureDirectory(const std::wstring& path)
    {
        if (path.empty() || DirectoryExists(path)) return;
        size_t split = path.find_last_of(L"\\/");
        if (split != std::wstring::npos) EnsureDirectory(path.substr(0, split));
        CreateDirectoryW(path.c_str(), nullptr);
    }

    std::string WideToUtf8(const std::wstring& value)
    {
        if (value.empty()) return "";
        int length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string output(length ? length - 1 : 0, '\0');
        if (length > 1)
        {
            WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, output.data(), length, nullptr, nullptr);
        }
        return output;
    }

    void WriteUtf8File(const std::wstring& path, const std::wstring& content)
    {
        EnsureDirectory(DirectoryName(path));
        std::string utf8 = WideToUtf8(content);
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
        WriteFile(file, bom, sizeof(bom), &written, nullptr);
        if (!utf8.empty())
        {
            WriteFile(file, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
        }
        CloseHandle(file);
    }

    void AppendU32(std::vector<unsigned char>& output, uint32_t value)
    {
        output.push_back((unsigned char)(value & 0xffu));
        output.push_back((unsigned char)((value >> 8) & 0xffu));
        output.push_back((unsigned char)((value >> 16) & 0xffu));
        output.push_back((unsigned char)((value >> 24) & 0xffu));
    }

    void AppendVarUInt(std::vector<unsigned char>& output, uint32_t value)
    {
        while (value >= 0x80u)
        {
            output.push_back((unsigned char)((value & 0x7fu) | 0x80u));
            value >>= 7;
        }
        output.push_back((unsigned char)value);
    }

    std::vector<unsigned char> CompressLzss(const std::vector<unsigned char>& input)
    {
        std::vector<unsigned char> output;
        size_t position = 0;
        while (position < input.size())
        {
            size_t flagPosition = output.size();
            output.push_back(0);
            unsigned char flags = 0;
            for (unsigned int bit = 0; bit < 8 && position < input.size(); ++bit)
            {
                size_t bestLength = 0;
                size_t bestOffset = 0;
                size_t windowStart = position > 4095 ? position - 4095 : 0;
                for (size_t candidate = windowStart; candidate < position; ++candidate)
                {
                    size_t length = 0;
                    while (length < 18
                        && position + length < input.size()
                        && input[candidate + length] == input[position + length])
                    {
                        ++length;
                    }
                    if (length > bestLength && length >= 3)
                    {
                        bestLength = length;
                        bestOffset = position - candidate;
                        if (bestLength == 18) break;
                    }
                }

                if (bestLength >= 3)
                {
                    flags |= (unsigned char)(1u << bit);
                    uint16_t token = (uint16_t)(((bestOffset & 0x0fffu) << 4) | ((bestLength - 3) & 0x0fu));
                    output.push_back((unsigned char)(token & 0xffu));
                    output.push_back((unsigned char)(token >> 8));
                    position += bestLength;
                }
                else
                {
                    output.push_back(input[position++]);
                }
            }
            output[flagPosition] = flags;
        }
        return output;
    }

    void WriteBinaryFile(const std::wstring& path, const std::vector<unsigned char>& content)
    {
        EnsureDirectory(DirectoryName(path));
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        if (!content.empty())
        {
            WriteFile(file, content.data(), (DWORD)content.size(), &written, nullptr);
        }
        CloseHandle(file);
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

    void ScanFiles(const std::wstring& root,
                   const std::wstring& relativeDirectory,
                   std::vector<std::wstring>& resourcePaths,
                   std::set<std::wstring>& directoryNames,
                   std::set<std::wstring>& fileNames)
    {
        std::wstring current = relativeDirectory.empty() ? root : Combine(root, relativeDirectory);
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileW(Combine(current, L"*").c_str(), &data);
        if (find == INVALID_HANDLE_VALUE) return;

        do
        {
            if (IsDotEntry(data.cFileName)) continue;
            std::wstring childRelative = relativeDirectory.empty() ? data.cFileName : Combine(relativeDirectory, data.cFileName);
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                directoryNames.insert(childRelative);
                ScanFiles(root, childRelative, resourcePaths, directoryNames, fileNames);
                continue;
            }

            if (wcscmp(data.cFileName, L"RestoreReport.txt") == 0 && relativeDirectory.empty())
            {
                continue;
            }

            resourcePaths.push_back(childRelative);
            fileNames.insert(data.cFileName);
            if (!relativeDirectory.empty())
            {
                directoryNames.insert(relativeDirectory);
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }

    std::wstring JoinLines(const std::vector<std::wstring>& lines)
    {
        std::wstring content;
        for (const auto& line : lines)
        {
            content += line + L"\r\n";
        }
        return content;
    }

    unsigned int CountRestoredFiles(const std::wstring& restoredDirectory)
    {
        std::vector<std::wstring> resourcePaths;
        std::set<std::wstring> directoryNames;
        std::set<std::wstring> fileNames;
        ScanFiles(restoredDirectory, L"", resourcePaths, directoryNames, fileNames);
        return (unsigned int)resourcePaths.size();
    }

    std::wstring PickRestoredDirectory(const std::wstring& gameDirectory)
    {
        std::wstring best;
        unsigned int bestCount = 0;
        for (int mode = 1; mode <= 3; ++mode)
        {
            std::wstring candidate = Combine(Combine(Combine(gameDirectory, L"User"), std::to_wstring(mode)), L"Restored_Extractor_Output");
            if (!DirectoryExists(candidate)) continue;
            unsigned int count = CountRestoredFiles(candidate);
            if (count > bestCount)
            {
                best = candidate;
                bestCount = count;
            }
        }

        std::wstring rootCandidate = Combine(gameDirectory, L"Restored_Extractor_Output");
        if (DirectoryExists(rootCandidate))
        {
            unsigned int count = CountRestoredFiles(rootCandidate);
            if (count > bestCount)
            {
                best = rootCandidate;
            }
        }
        return best;
    }

    std::wstring Trim(std::wstring value)
    {
        while (!value.empty() && (value.back() == L'\r' || value.back() == L'\n' || value.back() == L' ' || value.back() == L'\t'))
        {
            value.pop_back();
        }
        size_t start = 0;
        while (start < value.size() && (value[start] == L' ' || value[start] == L'\t'))
        {
            ++start;
        }
        return value.substr(start);
    }

    std::wstring FindHashSeedFromReport(const std::wstring& path)
    {
        std::wstring content;
        if (!ReadUtf16File(path, content)) return L"";
        const std::wstring markers[] = { L"HashSeed:", L"Hash Seed:", L"HashSeed：", L"Hash Seed：" };
        for (const auto& marker : markers)
        {
            size_t pos = content.find(marker);
            if (pos == std::wstring::npos)
            {
                continue;
            }
            size_t start = pos + marker.size();
            size_t end = content.find_first_of(L"\r\n", start);
            std::wstring value = Trim(content.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
            if (!value.empty())
            {
                return value;
            }
        }
        return L"";
    }

    std::wstring PickHashSeed(const std::wstring& gameDirectory)
    {
        for (int mode = 1; mode <= 3; ++mode)
        {
            std::wstring report = Combine(Combine(Combine(Combine(gameDirectory, L"User"), std::to_wstring(mode)), L"StaticHash_Output"), L"StaticHashReport.txt");
            std::wstring seed = FindHashSeedFromReport(report);
            if (!seed.empty())
            {
                return seed;
            }
            report = Combine(Combine(Combine(Combine(gameDirectory, L"User"), std::to_wstring(mode)), L"StringHashDumper_Output"), L"Universal.log");
            seed = FindHashSeedFromReport(report);
            if (!seed.empty())
            {
                return seed;
            }
        }
        return L"";
    }

    RestoreStats ReadRestoreStats(const std::wstring& restoredDirectory)
    {
        RestoreStats stats{};
        std::wstring reportPath = Combine(restoredDirectory, L"RestoreReport.txt");
        std::wstring content;
        if (!ReadUtf16File(reportPath, content))
        {
            return stats;
        }

        const std::wstring totalMarkers[] = { L"总文件数：", L"总文件数:", L"TOTAL_FILES: " };
        const std::wstring restoredMarkers[] = { L"成功还原：", L"成功还原:", L"RESTORED_FILES: " };

        auto readNumberAfter = [&](const std::wstring& marker) -> unsigned int
        {
            size_t pos = content.find(marker);
            if (pos == std::wstring::npos)
            {
                return 0u;
            }
            pos += marker.size();
            size_t end = pos;
            while (end < content.size() && iswdigit(content[end]))
            {
                ++end;
            }
            return (unsigned int)_wtoi(content.substr(pos, end - pos).c_str());
        };

        for (const auto& marker : totalMarkers)
        {
            stats.totalFiles = readNumberAfter(marker);
            if (stats.totalFiles > 0u)
            {
                break;
            }
        }
        for (const auto& marker : restoredMarkers)
        {
            stats.restoredFiles = readNumberAfter(marker);
            if (stats.restoredFiles > 0u)
            {
                break;
            }
        }

        stats.hasStats = stats.totalFiles > 0u && stats.restoredFiles <= stats.totalFiles;
        return stats;
    }

    std::wstring ToLower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), towlower);
        return value;
    }

    bool IsDigits(const std::wstring& text)
    {
        return !text.empty() && std::all_of(text.begin(), text.end(), iswdigit);
    }

    bool TryMatchVoicePattern(const std::wstring& resourcePath,
                              std::wstring& root,
                              std::wstring& prefix,
                              std::wstring& digits,
                              std::wstring& suffix,
                              std::wstring& extension)
    {
        std::wstring normalized = resourcePath;
        std::replace(normalized.begin(), normalized.end(), L'/', L'\\');

        size_t fileSlash = normalized.find_last_of(L'\\');
        if (fileSlash == std::wstring::npos || fileSlash == 0)
        {
            return false;
        }
        size_t dirSlash = normalized.find_last_of(L'\\', fileSlash - 1);
        if (dirSlash == std::wstring::npos)
        {
            return false;
        }

        root = normalized.substr(0, dirSlash + 1);
        prefix = normalized.substr(dirSlash + 1, fileSlash - dirSlash - 1);
        std::wstring fileName = normalized.substr(fileSlash + 1);
        size_t dot = fileName.find_last_of(L'.');
        if (dot == std::wstring::npos || dot == 0)
        {
            return false;
        }
        extension = ToLower(fileName.substr(dot));
        if (extension != L".ogg" && extension != L".wav" && extension != L".mp3")
        {
            return false;
        }

        std::wstring stem = fileName.substr(0, dot);
        if (stem.rfind(prefix, 0) != 0 || stem.size() <= prefix.size())
        {
            return false;
        }

        std::wstring rest = stem.substr(prefix.size());
        size_t digitStart = 0u;
        while (digitStart < rest.size() && !iswdigit(rest[digitStart]))
        {
            ++digitStart;
        }
        if (digitStart != 0u)
        {
            return false;
        }

        size_t digitEnd = digitStart;
        while (digitEnd < rest.size() && iswdigit(rest[digitEnd]))
        {
            ++digitEnd;
        }
        digits = rest.substr(digitStart, digitEnd - digitStart);
        suffix = rest.substr(digitEnd);
        return IsDigits(digits);
    }

    bool InferVoicePattern(const std::vector<std::wstring>& resourcePaths, VoicePatternCandidate& candidate)
    {
        std::map<std::wstring, VoicePatternCandidate> candidates;
        for (const auto& path : resourcePaths)
        {
            std::wstring root;
            std::wstring prefix;
            std::wstring digits;
            std::wstring suffix;
            std::wstring extension;
            if (!TryMatchVoicePattern(path, root, prefix, digits, suffix, extension))
            {
                continue;
            }

            std::wstring key = root + L"|" + extension + L"|" + std::to_wstring((unsigned int)digits.size());
            VoicePatternCandidate& entry = candidates[key];
            entry.root = root;
            entry.extension = extension;
            entry.numberWidth = (unsigned int)digits.size();
            entry.matchCount += 1u;
            entry.prefixes.insert(prefix);
            entry.suffixes.insert(suffix);
            unsigned int value = (unsigned int)_wtoi(digits.c_str());
            auto it = entry.prefixMaxNumbers.find(prefix);
            if (it == entry.prefixMaxNumbers.end() || value > it->second)
            {
                entry.prefixMaxNumbers[prefix] = value;
            }
            if (value > entry.maxNumber)
            {
                entry.maxNumber = value;
            }
        }

        VoicePatternCandidate best{};
        for (const auto& item : candidates)
        {
            const VoicePatternCandidate& current = item.second;
            if (current.matchCount < 8u || current.prefixes.size() < 2u)
            {
                continue;
            }
            if (current.matchCount > best.matchCount)
            {
                best = current;
            }
        }

        if (best.matchCount == 0u)
        {
            return false;
        }

        candidate = best;
        return true;
    }

    std::wstring JoinCsv(const std::set<std::wstring>& values)
    {
        std::wstring output;
        bool first = true;
        for (const auto& value : values)
        {
            if (!first)
            {
                output += L",";
            }
            output += value;
            first = false;
        }
        return output;
    }

    std::wstring JoinPrefixRanges(const std::map<std::wstring, unsigned int>& ranges)
    {
        std::wstring output;
        bool first = true;
        for (const auto& item : ranges)
        {
            if (!first)
            {
                output += L",";
            }
            output += item.first + L":" + std::to_wstring(item.second);
            first = false;
        }
        return output;
    }

    size_t CommonPrefixLength(const std::wstring& left, const std::wstring& right)
    {
        size_t count = 0;
        size_t limit = min(left.size(), right.size());
        while (count < limit && left[count] == right[count])
        {
            ++count;
        }
        return count;
    }

    std::wstring JoinFrontCodedPaths(const std::vector<std::wstring>& paths)
    {
        std::wstring content;
        std::wstring previous;
        for (const auto& path : paths)
        {
            size_t common = CommonPrefixLength(previous, path);
            content += std::to_wstring((unsigned int)common) + L"\t" + path.substr(common) + L"\r\n";
            previous = path;
        }
        return content;
    }

    std::wstring JoinLines(const std::set<std::wstring>& lines)
    {
        std::wstring content;
        for (const auto& line : lines)
        {
            content += line + L"\r\n";
        }
        return content;
    }

    std::vector<unsigned char> BuildRulesIntV2(const std::wstring& metaText,
                                               const std::wstring& patternText,
                                               const std::vector<std::wstring>& literalPaths)
    {
        std::string meta = WideToUtf8(metaText);
        std::string patterns = WideToUtf8(patternText);
        std::vector<unsigned char> pathBlock;
        std::wstring previous;
        for (const auto& path : literalPaths)
        {
            size_t common = CommonPrefixLength(previous, path);
            std::string suffix = WideToUtf8(path.substr(common));
            AppendVarUInt(pathBlock, (uint32_t)common);
            AppendVarUInt(pathBlock, (uint32_t)suffix.size());
            pathBlock.insert(pathBlock.end(), suffix.begin(), suffix.end());
            previous = path;
        }

        std::vector<unsigned char> output;
        std::vector<unsigned char> compressedPathBlock = CompressLzss(pathBlock);
        bool useCompressedPathBlock = !compressedPathBlock.empty() && compressedPathBlock.size() < pathBlock.size();

        const unsigned char magic[] = { 'C', 'X', 'R', 'I', '3', 0 };
        output.insert(output.end(), std::begin(magic), std::end(magic));
        AppendU32(output, (uint32_t)meta.size());
        AppendU32(output, (uint32_t)patterns.size());
        AppendU32(output, (uint32_t)literalPaths.size());
        AppendU32(output, (uint32_t)pathBlock.size());
        AppendU32(output, useCompressedPathBlock ? 1u : 0u);
        AppendU32(output, (uint32_t)(useCompressedPathBlock ? compressedPathBlock.size() : pathBlock.size()));
        output.insert(output.end(), meta.begin(), meta.end());
        output.insert(output.end(), patterns.begin(), patterns.end());
        const auto& storedPathBlock = useCompressedPathBlock ? compressedPathBlock : pathBlock;
        output.insert(output.end(), storedPathBlock.begin(), storedPathBlock.end());
        return output;
    }

    bool TryMatchGenericNumberPattern(const std::wstring& resourcePath,
                                      std::wstring& pattern,
                                      std::wstring& digits,
                                      std::wstring& digits2,
                                      std::wstring& suffix)
    {
        size_t slash = resourcePath.find_last_of(L"\\/");
        std::wstring directory = slash == std::wstring::npos ? L"" : resourcePath.substr(0, slash + 1);
        std::wstring fileName = slash == std::wstring::npos ? resourcePath : resourcePath.substr(slash + 1);
        std::vector<std::pair<size_t, size_t>> digitRuns;
        for (size_t index = 0; index < fileName.size();)
        {
            if (!iswdigit(fileName[index]))
            {
                ++index;
                continue;
            }
            size_t start = index;
            while (index < fileName.size() && iswdigit(fileName[index]))
            {
                ++index;
            }
            digitRuns.push_back({ start, index });
        }
        if (digitRuns.empty())
        {
            return false;
        }
        size_t digitStart = digitRuns.back().first;
        size_t digitEnd = digitRuns.back().second;
        size_t secondStart = std::wstring::npos;
        size_t secondEnd = std::wstring::npos;
        std::wstring lower = fileName;
        std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
        if (lower.rfind(L"ev", 0) == 0 || lower.rfind(L"bgm", 0) == 0 || lower.rfind(L"bg", 0) == 0)
        {
            digitStart = digitRuns.front().first;
            digitEnd = digitRuns.front().second;
        }
        if (digitStart == 0 || digitEnd == digitStart)
        {
            return false;
        }
        std::wstring head = fileName.substr(0, digitStart);
        digits = fileName.substr(digitStart, digitEnd - digitStart);
        if (secondStart != std::wstring::npos)
        {
            std::wstring middle = fileName.substr(digitEnd, secondStart - digitEnd);
            digits2 = fileName.substr(secondStart, secondEnd - secondStart);
            suffix = fileName.substr(secondEnd);
            pattern = directory + head + L"{num}" + middle + L"{num2}{suffix}";
        }
        else
        {
            suffix = fileName.substr(digitEnd);
            pattern = directory + head + L"{num}{suffix}";
        }
        if (suffix.empty())
        {
            return false;
        }
        return true;
    }

    std::vector<GenericPatternCandidate> InferGenericPatterns(const std::vector<std::wstring>& resourcePaths)
    {
        std::map<std::wstring, GenericPatternCandidate> candidates;
        for (const auto& path : resourcePaths)
        {
            std::wstring pattern;
            std::wstring digits;
            std::wstring digits2;
            std::wstring suffix;
            if (!TryMatchGenericNumberPattern(path, pattern, digits, digits2, suffix))
            {
                continue;
            }
            std::wstring key = pattern + L"|" + std::to_wstring((unsigned int)digits.size()) + L"|" + std::to_wstring((unsigned int)digits2.size());
            GenericPatternCandidate& candidate = candidates[key];
            candidate.pattern = pattern;
            candidate.numberWidth = (unsigned int)digits.size();
            candidate.numberWidth2 = digits2.empty() ? 0u : (unsigned int)digits2.size();
            candidate.hasSecondNumber = !digits2.empty();
            candidate.suffixes.insert(suffix);
            candidate.coveredPaths.insert(path);
            unsigned int value = (unsigned int)_wtoi(digits.c_str());
            if (value > candidate.maxNumber)
            {
                candidate.maxNumber = value;
            }
            if (!digits2.empty())
            {
                unsigned int value2 = (unsigned int)_wtoi(digits2.c_str());
                if (value2 > candidate.maxNumber2)
                {
                    candidate.maxNumber2 = value2;
                }
            }
        }

        std::vector<GenericPatternCandidate> accepted;
        for (auto& item : candidates)
        {
            GenericPatternCandidate& candidate = item.second;
            if (candidate.coveredPaths.size() < 8 || candidate.maxNumber == 0 || candidate.suffixes.empty())
            {
                continue;
            }
            if (candidate.hasSecondNumber)
            {
                continue;
            }
            candidate.generatedCount = (candidate.maxNumber + 1u) * (unsigned int)candidate.suffixes.size();
            if (candidate.hasSecondNumber)
            {
                candidate.generatedCount *= (candidate.maxNumber2 + 1u);
            }
            unsigned int suffixCount = (unsigned int)candidate.suffixes.size();
            if (candidate.generatedCount <= candidate.coveredPaths.size() * 2u && suffixCount <= max(8u, (unsigned int)candidate.coveredPaths.size() / 2u))
            {
                accepted.push_back(candidate);
            }
        }
        std::sort(accepted.begin(), accepted.end(), [](const GenericPatternCandidate& left, const GenericPatternCandidate& right)
        {
            return left.coveredPaths.size() > right.coveredPaths.size();
        });
        return accepted;
    }

    void DeletePathRecursive(const std::wstring& path)
    {
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileW(Combine(path, L"*").c_str(), &data);
        if (find != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (IsDotEntry(data.cFileName)) continue;
                std::wstring child = Combine(path, data.cFileName);
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                {
                    DeletePathRecursive(child);
                }
                else
                {
                    SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                    DeleteFileW(child.c_str());
                }
            } while (FindNextFileW(find, &data));
            FindClose(find);
        }
        RemoveDirectoryW(path.c_str());
    }

    void ClearDraftDirectory(const std::wstring& draftDirectory)
    {
        DeleteFileW(Combine(draftDirectory, L"manifest.ini").c_str());
        DeleteFileW(Combine(draftDirectory, L"rules.ini").c_str());
        DeleteFileW(Combine(draftDirectory, L"README.txt").c_str());
        DeleteFileW(Combine(draftDirectory, L"PublisherDraftReport.txt").c_str());
        DeletePathRecursive(Combine(draftDirectory, L"StaticHash_Input"));
    }

    int ComputeRestoreRatePercent(const RestoreStats& stats)
    {
        if (!stats.hasStats || stats.totalFiles == 0u)
        {
            return 0;
        }
        return (int)(((unsigned long long)stats.restoredFiles * 100ull) / stats.totalFiles);
    }

    unsigned int VoicePatternGeneratedCount(const VoicePatternCandidate& pattern)
    {
        if (pattern.matchCount == 0u || pattern.suffixes.empty())
        {
            return 0u;
        }
        if (!pattern.prefixMaxNumbers.empty())
        {
            unsigned long long total = 0;
            for (const auto& item : pattern.prefixMaxNumbers)
            {
                total += ((unsigned long long)item.second + 1ull) * (unsigned long long)pattern.suffixes.size();
                if (total > 1000000ull) return 1000000u;
            }
            return (unsigned int)total;
        }
        return (pattern.maxNumber + 1u) * (unsigned int)pattern.prefixes.size() * (unsigned int)pattern.suffixes.size();
    }

    bool ShouldUseMinimalVoicePackage(const VoicePatternCandidate& pattern,
                                      unsigned int resourcePathCount,
                                      unsigned int supplementalFileNameCount)
    {
        if (pattern.matchCount < 1000u)
        {
            return false;
        }
        if (resourcePathCount == 0 || pattern.matchCount * 100u / resourcePathCount < 45u)
        {
            return false;
        }
        if (supplementalFileNameCount > 0u && supplementalFileNameCount * 100u / resourcePathCount > 35u)
        {
            return false;
        }
        unsigned int generated = VoicePatternGeneratedCount(pattern);
        return generated > 0u && generated <= 250000u;
    }

    void AddVoicePatternPaths(const VoicePatternCandidate& pattern, std::set<std::wstring>& paths)
    {
        if (pattern.matchCount == 0u || pattern.suffixes.empty())
        {
            return;
        }
        for (const auto& item : pattern.prefixMaxNumbers)
        {
            const std::wstring& prefix = item.first;
            for (unsigned int number = 0; number <= item.second; ++number)
            {
                wchar_t numberText[32]{};
                swprintf_s(numberText, L"%0*u", pattern.numberWidth, number);
                for (const auto& suffix : pattern.suffixes)
                {
                    paths.insert(pattern.root + prefix + L"\\" + prefix + numberText + suffix + pattern.extension);
                }
            }
        }
    }
}

bool PublisherExtensionBuilderLite::BuildFromGameDirectory(const std::wstring& gameExePath,
                                                           const std::wstring& draftDirectory,
                                                           const std::wstring& brand,
                                                           Result& result,
                                                           std::wstring& errorMessage) const
{
    result = Result{};
    errorMessage.clear();

    std::wstring gameDirectory = DirectoryName(gameExePath);
    std::wstring restoredDirectory = PickRestoredDirectory(gameDirectory);
    if (restoredDirectory.empty())
    {
        errorMessage = L"找不到可用于制作扩展集的 Restored_Extractor_Output。";
        return false;
    }

    std::vector<std::wstring> resourcePaths;
    std::set<std::wstring> directoryNames;
    std::set<std::wstring> fileNames;
    ScanFiles(restoredDirectory, L"", resourcePaths, directoryNames, fileNames);
    std::sort(resourcePaths.begin(), resourcePaths.end());

    if (resourcePaths.empty())
    {
        errorMessage = L"Restored_Extractor_Output 中没有可用文件。";
        return false;
    }

    std::wstring gameName = FileStem(gameExePath);
    std::wstring brandName = brand;
    std::wstring hashSeed = PickHashSeed(gameDirectory);
    RestoreStats stats = ReadRestoreStats(restoredDirectory);
    VoicePatternCandidate voicePattern{};
    bool hasVoicePattern = InferVoicePattern(resourcePaths, voicePattern);
    std::set<std::wstring> voiceCoveredPaths;
    std::set<std::wstring> supplementalFileNames;
    if (hasVoicePattern)
    {
        AddVoicePatternPaths(voicePattern, voiceCoveredPaths);
        for (const auto& path : resourcePaths)
        {
            if (voiceCoveredPaths.find(path) != voiceCoveredPaths.end())
            {
                continue;
            }
            size_t slash = path.find_last_of(L"\\/");
            std::wstring fileName = slash == std::wstring::npos ? path : path.substr(slash + 1);
            if (!fileName.empty())
            {
                supplementalFileNames.insert(fileName);
            }
        }
    }
    bool useMinimalVoicePackage = hasVoicePattern && ShouldUseMinimalVoicePackage(voicePattern, (unsigned int)resourcePaths.size(), (unsigned int)supplementalFileNames.size());
    std::vector<GenericPatternCandidate> genericPatterns = InferGenericPatterns(resourcePaths);
    std::set<std::wstring> extraDirectories;
    if (useMinimalVoicePackage)
    {
        genericPatterns.clear();
        for (const auto& directory : directoryNames)
        {
            if (!directory.empty())
            {
                extraDirectories.insert(directory);
            }
            size_t split = directory.find_first_of(L"\\/");
            if (split != std::wstring::npos)
            {
                extraDirectories.insert(directory.substr(split + 1));
            }
        }
    }
    std::set<std::wstring> coveredByRules;
    for (const auto& pattern : genericPatterns)
    {
        coveredByRules.insert(pattern.coveredPaths.begin(), pattern.coveredPaths.end());
    }
    std::vector<std::wstring> literalPaths;
    for (const auto& path : resourcePaths)
    {
        if (!useMinimalVoicePackage && coveredByRules.find(path) == coveredByRules.end())
        {
            literalPaths.push_back(path);
        }
    }
    if (useMinimalVoicePackage)
    {
        for (const auto& fileName : supplementalFileNames)
        {
            if (!fileName.empty())
            {
                literalPaths.push_back(L"|F|" + fileName);
            }
        }
    }

    EnsureDirectory(draftDirectory);
    ClearDraftDirectory(draftDirectory);

    std::wstring manifest;
    manifest += L"CXDEC-MANIFEST-INT\t1\r\n";
    manifest += L"[Meta]\r\n";
    if (!brandName.empty())
    {
        manifest += L"Brand=" + brandName + L"\r\n";
    }
    manifest += L"Game=" + gameName + L"\r\n";
    manifest += L"HashSeed=" + hashSeed + L"\r\n";
    manifest += L"Format=manifest.int\r\n";
    manifest += L"PackageMode=" + std::wstring(useMinimalVoicePackage ? L"minimal-voice-rules" : L"int-rules-lz") + L"\r\n";
    manifest += L"ResourcePaths=" + std::to_wstring((unsigned int)resourcePaths.size()) + L"\r\n";
    manifest += L"Summary=自动生成的新版 int 扩展集，规则优先，例外路径使用前缀压缩保存。\r\n";
    if (stats.hasStats)
    {
        manifest += L"TotalFiles=" + std::to_wstring(stats.totalFiles) + L"\r\n";
        manifest += L"RestoredFiles=" + std::to_wstring(stats.restoredFiles) + L"\r\n";
        manifest += L"RestoreRate=" + std::to_wstring(ComputeRestoreRatePercent(stats)) + L"\r\n";
    }
    WriteUtf8File(Combine(draftDirectory, L"manifest.int"), manifest);

    std::wstring rulesMeta;
    rulesMeta += L"[Meta]\r\n";
    if (!brandName.empty())
    {
        rulesMeta += L"Brand=" + brandName + L"\r\n";
    }
    rulesMeta += L"GameId=" + gameName + L"\r\n";
    rulesMeta += L"GameDisplayName=" + gameName + L"\r\n";
    rulesMeta += L"HashSeed=" + hashSeed + L"\r\n";
    rulesMeta += L"Encoding=UTF-16LE\r\n";
    if (stats.hasStats)
    {
        rulesMeta += L"MinRestoreRate=" + std::to_wstring(ComputeRestoreRatePercent(stats)) + L"\r\n";
    }
    std::wstring rulesPatterns;
    if (hasVoicePattern)
    {
        rulesPatterns += L"\r\n[Pattern:Voice]\r\n";
        rulesPatterns += L"Pattern=" + voicePattern.root + L"{prefix}\\{prefix}{num}{suffix}" + voicePattern.extension + L"\r\n";
        rulesPatterns += L"Prefixes=" + JoinCsv(voicePattern.prefixes) + L"\r\n";
        rulesPatterns += L"PrefixRanges=" + JoinPrefixRanges(voicePattern.prefixMaxNumbers) + L"\r\n";
        rulesPatterns += L"MaxNumber=" + std::to_wstring(voicePattern.maxNumber) + L"\r\n";
        rulesPatterns += L"NumberWidth=" + std::to_wstring(voicePattern.numberWidth) + L"\r\n";
        rulesPatterns += L"Suffixes=" + JoinCsv(voicePattern.suffixes) + L"\r\n";
    }
    unsigned int patternIndex = 1;
    for (const auto& pattern : genericPatterns)
    {
        rulesPatterns += L"\r\n[Pattern:Generic" + std::to_wstring(patternIndex++) + L"]\r\n";
        rulesPatterns += L"Pattern=" + pattern.pattern + L"\r\n";
        rulesPatterns += L"Prefixes=_\r\n";
        rulesPatterns += L"PrefixRanges=_:" + std::to_wstring(pattern.maxNumber) + L"\r\n";
        rulesPatterns += L"MaxNumber=" + std::to_wstring(pattern.maxNumber) + L"\r\n";
        rulesPatterns += L"NumberWidth=" + std::to_wstring(pattern.numberWidth) + L"\r\n";
        if (pattern.hasSecondNumber)
        {
            rulesPatterns += L"MaxNumber2=" + std::to_wstring(pattern.maxNumber2) + L"\r\n";
            rulesPatterns += L"NumberWidth2=" + std::to_wstring(pattern.numberWidth2) + L"\r\n";
        }
        rulesPatterns += L"Suffixes=" + JoinCsv(pattern.suffixes) + L"\r\n";
    }
    if (!extraDirectories.empty())
    {
        rulesPatterns += L"\r\n[Directories]\r\n";
        rulesPatterns += JoinLines(extraDirectories);
    }
    WriteBinaryFile(Combine(draftDirectory, L"rules.int"), BuildRulesIntV2(rulesMeta, rulesPatterns, literalPaths));

    result.resourcePathCount = (unsigned int)resourcePaths.size();
    result.directoryNameCount = (unsigned int)directoryNames.size();
    result.fileNameCount = (unsigned int)fileNames.size();
    result.minimalPackage = true;
    result.usedStaticInput = false;
    result.sourceRestoredDirectory = restoredDirectory;
    result.draftDirectory = draftDirectory;
    result.reportPath = Combine(draftDirectory, L"rules.int");

    std::wstring report;
    report += L"发布者扩展集生成报告\r\n\r\n";
    report += L"来源目录: " + restoredDirectory + L"\r\n";
    report += L"草稿目录: " + draftDirectory + L"\r\n";
    report += L"资源路径数: " + std::to_wstring(result.resourcePathCount) + L"\r\n";
    report += L"目录候选数: " + std::to_wstring(result.directoryNameCount) + L"\r\n";
    report += L"文件名候选数: " + std::to_wstring(result.fileNameCount) + L"\r\n";
    report += L"HashSeed: " + (hashSeed.empty() ? L"(未找到，需要人工补写)" : hashSeed) + L"\r\n";
    report += L"发布模式: " + std::wstring(useMinimalVoicePackage ? L"minimal-voice-rules" : L"manifest.int + rules.int") + L"\r\n";
    report += L"通用规则数: " + std::to_wstring((unsigned int)genericPatterns.size()) + L"\r\n";
    report += L"压缩兜底路径数: " + std::to_wstring((unsigned int)literalPaths.size()) + L"\r\n";
    if (hasVoicePattern)
    {
        report += L"自动规则: 已推导出 VoicePattern，前缀数 " + std::to_wstring((unsigned int)voicePattern.prefixes.size()) + L"\r\n";
    }
    else
    {
        report += L"自动规则: 未推导出稳定 VoicePattern，已使用 rules.int 前缀压缩例外段兜底。\r\n";
    }
    return true;
}
