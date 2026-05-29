#include "StaticHashGenerator.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwchar>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "directory.h"
#include "file.h"
#include "path.h"
#include "stringhelper.h"
#include "util.h"

namespace
{
    constexpr const wchar_t StaticHashInputFolder[] = L"StaticHash_Input";
    constexpr const wchar_t StaticHashOutputFolder[] = L"StaticHash_Output";
    constexpr const wchar_t RestoredOutputFolder[] = L"Restored_Extractor_Output";
    constexpr const wchar_t StringHashOutputFolder[] = L"StringHashDumper_Output";
    constexpr const wchar_t UniversalFileName[] = L"Universal.log";
    constexpr const wchar_t ResourcePathsFileName[] = L"ResourcePaths.txt";
    constexpr const wchar_t DirectoryNamesFileName[] = L"DirectoryNames.txt";
    constexpr const wchar_t FileNamesFileName[] = L"FileNames.txt";
    constexpr const wchar_t DirectoryHashFileName[] = L"DirectoryHash.log";
    constexpr const wchar_t FileNameHashFileName[] = L"FileNameHash.log";
    constexpr const wchar_t StaticHashReportFileName[] = L"StaticHashReport.txt";
    constexpr const wchar_t EmptyDirectoryMarker[] = L"%EmptyString%";
    constexpr const wchar_t HashLogSplit[] = L"##YSig##";

    bool IsDotEntry(const wchar_t* name);

    uint32_t Load32(const uint8_t* p)
    {
        return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }

    void Store32(uint8_t* p, uint32_t v)
    {
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
        p[2] = (uint8_t)(v >> 16);
        p[3] = (uint8_t)(v >> 24);
    }

    void Store64(uint8_t* p, uint64_t v)
    {
        for (unsigned int i = 0; i < 8; ++i)
        {
            p[i] = (uint8_t)(v >> (i * 8));
        }
    }

    uint32_t Rotr32(uint32_t value, unsigned int bits)
    {
        return (value >> bits) | (value << (32u - bits));
    }

    uint64_t Rotl64(uint64_t value, unsigned int bits)
    {
        return (value << bits) | (value >> (64u - bits));
    }

    void Blake2sCompress(uint32_t state[8], const uint8_t block[64], uint64_t counter, bool last)
    {
        static constexpr uint32_t iv[8] =
        {
            0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
            0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u
        };
        static constexpr uint8_t sigma[10][16] =
        {
            { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
            { 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 },
            { 11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4 },
            { 7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8 },
            { 9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13 },
            { 2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9 },
            { 12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11 },
            { 13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10 },
            { 6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5 },
            { 10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0 }
        };

        uint32_t m[16]{};
        for (unsigned int i = 0; i < 16; ++i)
        {
            m[i] = Load32(block + i * 4);
        }

        uint32_t v[16]{};
        for (unsigned int i = 0; i < 8; ++i)
        {
            v[i] = state[i];
            v[i + 8] = iv[i];
        }

        v[12] ^= (uint32_t)counter;
        v[13] ^= (uint32_t)(counter >> 32);
        if (last)
        {
            v[14] = ~v[14];
        }

#define B2S_G(a, b, c, d, x, y) \
        do { \
            v[a] = v[a] + v[b] + (x); \
            v[d] = Rotr32(v[d] ^ v[a], 16); \
            v[c] = v[c] + v[d]; \
            v[b] = Rotr32(v[b] ^ v[c], 12); \
            v[a] = v[a] + v[b] + (y); \
            v[d] = Rotr32(v[d] ^ v[a], 8); \
            v[c] = v[c] + v[d]; \
            v[b] = Rotr32(v[b] ^ v[c], 7); \
        } while (0)

        for (unsigned int r = 0; r < 10; ++r)
        {
            B2S_G(0, 4, 8, 12, m[sigma[r][0]], m[sigma[r][1]]);
            B2S_G(1, 5, 9, 13, m[sigma[r][2]], m[sigma[r][3]]);
            B2S_G(2, 6, 10, 14, m[sigma[r][4]], m[sigma[r][5]]);
            B2S_G(3, 7, 11, 15, m[sigma[r][6]], m[sigma[r][7]]);
            B2S_G(0, 5, 10, 15, m[sigma[r][8]], m[sigma[r][9]]);
            B2S_G(1, 6, 11, 12, m[sigma[r][10]], m[sigma[r][11]]);
            B2S_G(2, 7, 8, 13, m[sigma[r][12]], m[sigma[r][13]]);
            B2S_G(3, 4, 9, 14, m[sigma[r][14]], m[sigma[r][15]]);
        }

#undef B2S_G

        for (unsigned int i = 0; i < 8; ++i)
        {
            state[i] ^= v[i] ^ v[i + 8];
        }
    }

    std::array<uint8_t, 32> Blake2s256(const std::vector<uint8_t>& input)
    {
        uint32_t state[8] =
        {
            0x6A09E667u ^ 0x01010020u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
            0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u
        };

        uint64_t counter = 0;
        size_t offset = 0;
        while (input.size() - offset > 64)
        {
            Blake2sCompress(state, input.data() + offset, counter + 64, false);
            counter += 64;
            offset += 64;
        }

        uint8_t block[64]{};
        size_t remaining = input.size() - offset;
        if (remaining)
        {
            memcpy(block, input.data() + offset, remaining);
        }
        counter += remaining;
        Blake2sCompress(state, block, counter, true);

        std::array<uint8_t, 32> digest{};
        for (unsigned int i = 0; i < 8; ++i)
        {
            Store32(digest.data() + i * 4, state[i]);
        }
        return digest;
    }

    uint64_t SipHash24(const std::vector<uint8_t>& input)
    {
        uint64_t v0 = 0x736f6d6570736575ull;
        uint64_t v1 = 0x646f72616e646f6dull;
        uint64_t v2 = 0x6c7967656e657261ull;
        uint64_t v3 = 0x7465646279746573ull;

#define SIP_ROUND() \
        do { \
            v0 += v1; v1 = Rotl64(v1, 13); v1 ^= v0; v0 = Rotl64(v0, 32); \
            v2 += v3; v3 = Rotl64(v3, 16); v3 ^= v2; \
            v0 += v3; v3 = Rotl64(v3, 21); v3 ^= v0; \
            v2 += v1; v1 = Rotl64(v1, 17); v1 ^= v2; v2 = Rotl64(v2, 32); \
        } while (0)

        size_t offset = 0;
        while (offset + 8 <= input.size())
        {
            uint64_t m = 0;
            for (unsigned int i = 0; i < 8; ++i)
            {
                m |= ((uint64_t)input[offset + i]) << (8 * i);
            }
            v3 ^= m;
            SIP_ROUND();
            SIP_ROUND();
            v0 ^= m;
            offset += 8;
        }

        uint64_t b = ((uint64_t)input.size()) << 56;
        for (unsigned int i = 0; offset + i < input.size(); ++i)
        {
            b |= ((uint64_t)input[offset + i]) << (8 * i);
        }

        v3 ^= b;
        SIP_ROUND();
        SIP_ROUND();
        v0 ^= b;
        v2 ^= 0xff;
        SIP_ROUND();
        SIP_ROUND();
        SIP_ROUND();
        SIP_ROUND();

#undef SIP_ROUND

        return v0 ^ v1 ^ v2 ^ v3;
    }

    std::wstring TrimLine(std::wstring value)
    {
        while (!value.empty() && (value.back() == L'\r' || value.back() == L'\n' || value.back() == L' ' || value.back() == L'\t'))
        {
            value.pop_back();
        }

        size_t start = 0;
        while (start < value.length() && (value[start] == L' ' || value[start] == L'\t'))
        {
            ++start;
        }

        return start == 0 ? value : value.substr(start);
    }

    bool ReadUtf16File(const std::wstring& path, std::wstring& output)
    {
        HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
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

        if (!success)
        {
            return false;
        }

        size_t offset = 0;
        if (buffer.size() >= 2 && buffer[0] == 0xff && buffer[1] == 0xfe)
        {
            offset = 2;
        }

        if (((buffer.size() - offset) % sizeof(wchar_t)) != 0)
        {
            return false;
        }

        output.assign((const wchar_t*)(buffer.data() + offset), (buffer.size() - offset) / sizeof(wchar_t));
        return true;
    }

    bool ReadTextLines(const std::wstring& path, std::set<std::wstring>& output, unsigned int& lineCount)
    {
        std::wstring content;
        if (!ReadUtf16File(path, content))
        {
            return false;
        }

        size_t start = 0;
        while (start <= content.length())
        {
            size_t end = content.find(L'\n', start);
            std::wstring line = TrimLine(content.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
            if (!line.empty() && line[0] != L'#')
            {
                output.insert(line);
                ++lineCount;
            }

            if (end == std::wstring::npos)
            {
                break;
            }
            start = end + 1;
        }

        return true;
    }

    bool TryReadSeed(const std::wstring& gameDirectory, std::wstring& seed)
    {
        std::wstring content;
        if (!ReadUtf16File(Path::Combine(Path::Combine(gameDirectory, StringHashOutputFolder), UniversalFileName), content))
        {
            return false;
        }

        const std::wstring prefix = L"Hash Seed:";
        size_t pos = content.find(prefix);
        if (pos == std::wstring::npos)
        {
            return false;
        }

        size_t start = pos + prefix.length();
        size_t end = content.find_first_of(L"\r\n", start);
        seed = TrimLine(content.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        return !seed.empty();
    }

    bool ReadAnsiFile(const std::wstring& path, std::string& output)
    {
        HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
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

        std::vector<char> buffer((size_t)size.QuadPart);
        DWORD read = 0;
        bool success = ::ReadFile(file, buffer.data(), (DWORD)buffer.size(), &read, nullptr) && read == buffer.size();
        ::CloseHandle(file);
        if (!success)
        {
            return false;
        }

        output.assign(buffer.begin(), buffer.end());
        return true;
    }

    std::wstring TrimCopy(const std::wstring& value)
    {
        return TrimLine(value);
    }

    std::wstring StripCopySuffix(std::wstring name)
    {
        const std::wstring suffix = L" - 副本";
        if (name.length() >= suffix.length() && name.substr(name.length() - suffix.length()) == suffix)
        {
            name.resize(name.length() - suffix.length());
        }
        return name;
    }

    std::wstring GetExtensionRoot()
    {
        std::wstring appDirectory = Path::GetDirectoryName(Util::GetAppPathW());
        std::wstring local = Path::Combine(appDirectory, L"\u6269\u5c55\u96c6");
        if (Directory::Exists(local))
        {
            return local;
        }

        return Path::Combine(Path::GetDirectoryName(appDirectory), L"\u6269\u5c55\u96c6");
    }

    bool TryCopyExtensionStaticInput(const std::wstring& gameDirectory)
    {
        std::wstring extensionRoot = GetExtensionRoot();
        if (!Directory::Exists(extensionRoot))
        {
            return false;
        }

        std::wstring gameName = StripCopySuffix(Path::GetFileName(gameDirectory));
        std::wstring targetInput = Path::Combine(gameDirectory, StaticHashInputFolder);
        std::wstring targetResourcePaths = Path::Combine(targetInput, ResourcePathsFileName);
        std::wstring targetDirectoryNames = Path::Combine(targetInput, DirectoryNamesFileName);
        std::wstring targetFileNames = Path::Combine(targetInput, FileNamesFileName);
        if (Path::Exists(targetResourcePaths) || Path::Exists(targetDirectoryNames) || Path::Exists(targetFileNames))
        {
            return false;
        }

        std::wstring brandSearch = Path::Combine(extensionRoot, L"*");
        WIN32_FIND_DATAW brandData{};
        HANDLE brandFind = ::FindFirstFileW(brandSearch.c_str(), &brandData);
        if (brandFind == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        bool copied = false;
        do
        {
            if (IsDotEntry(brandData.cFileName) || (brandData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                continue;
            }

            std::wstring brandPath = Path::Combine(extensionRoot, brandData.cFileName);
            std::wstring gameSearch = Path::Combine(brandPath, L"*");
            WIN32_FIND_DATAW gameData{};
            HANDLE gameFind = ::FindFirstFileW(gameSearch.c_str(), &gameData);
            if (gameFind == INVALID_HANDLE_VALUE)
            {
                continue;
            }

            do
            {
                if (IsDotEntry(gameData.cFileName) || (gameData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                {
                    continue;
                }

                std::wstring extensionGameName = gameData.cFileName;
                if (extensionGameName != gameName && gameName.find(extensionGameName) == std::wstring::npos && extensionGameName.find(gameName) == std::wstring::npos)
                {
                    continue;
                }

                std::wstring sourceInput = Path::Combine(Path::Combine(brandPath, gameData.cFileName), StaticHashInputFolder);
                if (!Directory::Exists(sourceInput))
                {
                    continue;
                }

                Directory::Create(targetInput);
                copied |= ::CopyFileW(Path::Combine(sourceInput, ResourcePathsFileName).c_str(), targetResourcePaths.c_str(), FALSE) != FALSE;
                copied |= ::CopyFileW(Path::Combine(sourceInput, DirectoryNamesFileName).c_str(), targetDirectoryNames.c_str(), FALSE) != FALSE;
                copied |= ::CopyFileW(Path::Combine(sourceInput, FileNamesFileName).c_str(), targetFileNames.c_str(), FALSE) != FALSE;
                break;
            } while (::FindNextFileW(gameFind, &gameData));

            ::FindClose(gameFind);
            if (copied)
            {
                break;
            }
        } while (::FindNextFileW(brandFind, &brandData));

        ::FindClose(brandFind);
        return copied;
    }

    bool TryReadSeedFromExtensionRules(const std::wstring& gameDirectory, std::wstring& seed)
    {
        std::wstring extensionRoot = GetExtensionRoot();
        if (!Directory::Exists(extensionRoot))
        {
            return false;
        }

        std::wstring gameName = StripCopySuffix(Path::GetFileName(gameDirectory));

        std::wstring brandSearch = Path::Combine(extensionRoot, L"*");
        WIN32_FIND_DATAW brandData{};
        HANDLE brandFind = ::FindFirstFileW(brandSearch.c_str(), &brandData);
        if (brandFind == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        bool found = false;
        do
        {
            if (IsDotEntry(brandData.cFileName) || (brandData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                continue;
            }

            std::wstring brandPath = Path::Combine(extensionRoot, brandData.cFileName);
            std::wstring gameSearch = Path::Combine(brandPath, L"*");
            WIN32_FIND_DATAW gameData{};
            HANDLE gameFind = ::FindFirstFileW(gameSearch.c_str(), &gameData);
            if (gameFind == INVALID_HANDLE_VALUE)
            {
                continue;
            }

            do
            {
                if (IsDotEntry(gameData.cFileName) || (gameData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                {
                    continue;
                }

                std::wstring extensionGameName = gameData.cFileName;
                if (extensionGameName != gameName && gameName.find(extensionGameName) == std::wstring::npos && extensionGameName.find(gameName) == std::wstring::npos)
                {
                    continue;
                }

                std::string contentA;
                if (!ReadAnsiFile(Path::Combine(Path::Combine(brandPath, gameData.cFileName), L"rules.ini"), contentA))
                {
                    continue;
                }

                std::wstring content(contentA.begin(), contentA.end());
                size_t pos = content.find(L"HashSeed=");
                if (pos == std::wstring::npos)
                {
                    continue;
                }

                size_t start = pos + wcslen(L"HashSeed=");
                size_t end = content.find_first_of(L"\r\n", start);
                seed = TrimCopy(content.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
                found = !seed.empty();
                break;
            } while (::FindNextFileW(gameFind, &gameData));

            ::FindClose(gameFind);
            if (found)
            {
                break;
            }
        } while (::FindNextFileW(brandFind, &brandData));

        ::FindClose(brandFind);
        return found;
    }

    std::vector<std::wstring> SplitList(const std::wstring& value)
    {
        std::vector<std::wstring> values;
        size_t start = 0;
        while (start <= value.length())
        {
            size_t end = value.find_first_of(L",;", start);
            values.push_back(TrimCopy(value.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start)));
            if (end == std::wstring::npos)
            {
                break;
            }
            start = end + 1;
        }
        return values;
    }

    std::map<std::wstring, std::wstring> ReadRuleValues(const std::wstring& path)
    {
        std::map<std::wstring, std::wstring> values;
        std::string contentA;
        if (!ReadAnsiFile(path, contentA))
        {
            return values;
        }

        std::wstring content(contentA.begin(), contentA.end());
        size_t start = 0;
        while (start <= content.length())
        {
            size_t end = content.find_first_of(L"\r\n", start);
            std::wstring line = TrimCopy(content.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
            if (!line.empty() && line[0] != L'[' && line[0] != L'#' && line[0] != L';')
            {
                size_t equals = line.find(L'=');
                if (equals != std::wstring::npos)
                {
                    values[TrimCopy(line.substr(0, equals))] = TrimCopy(line.substr(equals + 1));
                }
            }

            if (end == std::wstring::npos)
            {
                break;
            }
            start = end + 1;
        }
        return values;
    }

    std::wstring RuleValue(const std::map<std::wstring, std::wstring>& values, const wchar_t* key)
    {
        auto it = values.find(key);
        return it == values.end() ? L"" : it->second;
    }

    std::wstring ReplaceAll(std::wstring text, const std::wstring& from, const std::wstring& to)
    {
        size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::wstring::npos)
        {
            text.replace(pos, from.length(), to);
            pos += to.length();
        }
        return text;
    }

    bool TryGenerateExtensionStaticInput(const std::wstring& gameDirectory, std::set<std::wstring>& resourcePaths, unsigned int& count)
    {
        std::wstring extensionRoot = GetExtensionRoot();
        if (!Directory::Exists(extensionRoot))
        {
            return false;
        }

        std::wstring gameName = StripCopySuffix(Path::GetFileName(gameDirectory));
        std::wstring matchedRules;
        std::wstring brandSearch = Path::Combine(extensionRoot, L"*");
        WIN32_FIND_DATAW brandData{};
        HANDLE brandFind = ::FindFirstFileW(brandSearch.c_str(), &brandData);
        if (brandFind == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        do
        {
            if (IsDotEntry(brandData.cFileName) || (brandData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                continue;
            }

            std::wstring brandPath = Path::Combine(extensionRoot, brandData.cFileName);
            std::wstring gameSearch = Path::Combine(brandPath, L"*");
            WIN32_FIND_DATAW gameData{};
            HANDLE gameFind = ::FindFirstFileW(gameSearch.c_str(), &gameData);
            if (gameFind == INVALID_HANDLE_VALUE)
            {
                continue;
            }

            do
            {
                if (IsDotEntry(gameData.cFileName) || (gameData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                {
                    continue;
                }

                std::wstring extensionGameName = gameData.cFileName;
                if (extensionGameName == gameName || gameName.find(extensionGameName) != std::wstring::npos || extensionGameName.find(gameName) != std::wstring::npos)
                {
                    matchedRules = Path::Combine(Path::Combine(brandPath, gameData.cFileName), L"rules.ini");
                    break;
                }
            } while (::FindNextFileW(gameFind, &gameData));

            ::FindClose(gameFind);
            if (!matchedRules.empty())
            {
                break;
            }
        } while (::FindNextFileW(brandFind, &brandData));

        ::FindClose(brandFind);
        if (matchedRules.empty())
        {
            return false;
        }

        auto rules = ReadRuleValues(matchedRules);
        std::wstring voicePattern = RuleValue(rules, L"VoicePattern");
        if (voicePattern.empty())
        {
            voicePattern = RuleValue(rules, L"Pattern");
        }

        std::vector<std::wstring> prefixes = SplitList(RuleValue(rules, L"Prefixes"));
        std::vector<std::wstring> suffixes = SplitList(RuleValue(rules, L"Suffixes"));
        int maxNumber = _wtoi(RuleValue(rules, L"MaxNumber").c_str());
        int numberWidth = _wtoi(RuleValue(rules, L"NumberWidth").c_str());
        if (numberWidth <= 0)
        {
            numberWidth = 4;
        }

        bool generated = false;
        if (!voicePattern.empty() && !prefixes.empty() && maxNumber > 0)
        {
            for (const auto& prefix : prefixes)
            {
                if (prefix.empty())
                {
                    continue;
                }
                for (int number = 0; number <= maxNumber; ++number)
                {
                    wchar_t numberText[32]{};
                    swprintf_s(numberText, L"%0*d", numberWidth, number);
                    for (const auto& suffix : suffixes)
                    {
                        std::wstring path = voicePattern;
                        path = ReplaceAll(path, L"{prefix}", prefix);
                        path = ReplaceAll(path, L"{num}", numberText);
                        path = ReplaceAll(path, L"{suffix}", suffix);
                        if (resourcePaths.insert(path).second)
                        {
                            ++count;
                        }
                        generated = true;
                    }
                }
            }
        }

        return generated;
    }

    bool IsDotEntry(const wchar_t* name)
    {
        return wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0;
    }

    bool IsUnsafeInputPath(const std::wstring& path)
    {
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

    std::wstring NormalizeResourcePath(std::wstring path)
    {
        std::replace(path.begin(), path.end(), L'/', L'\\');
        while (!path.empty() && path.front() == L'\\')
        {
            path.erase(path.begin());
        }
        while (!path.empty() && path.back() == L'\\')
        {
            path.pop_back();
        }
        return path;
    }

    std::wstring NormalizeDirectoryHashName(std::wstring directory)
    {
        std::replace(directory.begin(), directory.end(), L'\\', L'/');
        while (!directory.empty() && directory.front() == L'/')
        {
            directory.erase(directory.begin());
        }
        while (!directory.empty() && directory.back() == L'/')
        {
            directory.pop_back();
        }
        if (!directory.empty())
        {
            directory.push_back(L'/');
        }
        return directory;
    }

    void SplitResourcePath(const std::wstring& resourcePath, std::set<std::wstring>& directories, std::set<std::wstring>& fileNames)
    {
        std::wstring path = NormalizeResourcePath(resourcePath);
        if (path.empty() || IsUnsafeInputPath(path))
        {
            return;
        }

        size_t split = path.find_last_of(L"\\/");
        if (split == std::wstring::npos)
        {
            directories.insert(L"");
            fileNames.insert(path);
            return;
        }

        std::wstring directory = NormalizeResourcePath(path.substr(0, split));
        std::wstring fileName = path.substr(split + 1);
        if (!fileName.empty())
        {
            directories.insert(NormalizeDirectoryHashName(directory));
            fileNames.insert(fileName);
        }
    }

    void AddRestoredPaths(const std::wstring& root,
                          const std::wstring& current,
                          const std::wstring& relative,
                          std::set<std::wstring>& resourcePaths,
                          unsigned int& count)
    {
        std::wstring searchPattern = Path::Combine(current, L"*");
        WIN32_FIND_DATAW data{};
        HANDLE find = ::FindFirstFileW(searchPattern.c_str(), &data);
        if (find == INVALID_HANDLE_VALUE)
        {
            return;
        }

        do
        {
            if (IsDotEntry(data.cFileName))
            {
                continue;
            }

            std::wstring childPath = Path::Combine(current, data.cFileName);
            std::wstring childRelative = relative.empty() ? data.cFileName : Path::Combine(relative, data.cFileName);
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                AddRestoredPaths(root, childPath, childRelative, resourcePaths, count);
            }
            else
            {
                size_t split = childRelative.find_first_of(L"\\/");
                std::wstring pathWithoutPackage = split == std::wstring::npos ? childRelative : childRelative.substr(split + 1);
                if (!pathWithoutPackage.empty())
                {
                    resourcePaths.insert(pathWithoutPackage);
                    ++count;
                }
            }
        } while (::FindNextFileW(find, &data));

        ::FindClose(find);
    }

    std::vector<uint8_t> MakeHashInput(const std::wstring& text, const std::wstring& seed)
    {
        std::vector<uint8_t> bytes((text.length() + seed.length()) * sizeof(wchar_t));
        if (!text.empty())
        {
            memcpy(bytes.data(), text.data(), text.length() * sizeof(wchar_t));
        }
        if (!seed.empty())
        {
            memcpy(bytes.data() + text.length() * sizeof(wchar_t), seed.data(), seed.length() * sizeof(wchar_t));
        }
        return bytes;
    }

    std::wstring FileNameHash(const std::wstring& fileName, const std::wstring& seed)
    {
        auto digest = Blake2s256(MakeHashInput(fileName, seed));
        return StringHelper::BytesToHexStringW(digest.data(), (unsigned __int32)digest.size());
    }

    std::wstring DirectoryHash(const std::wstring& directoryName, const std::wstring& seed)
    {
        uint8_t output[8]{};
        uint64_t value = SipHash24(MakeHashInput(NormalizeDirectoryHashName(directoryName), seed));
        Store64(output, value);
        return StringHelper::BytesToHexStringW(output, sizeof(output));
    }

    void AppendUtf16File(const std::wstring& path, const std::wstring& content)
    {
        WORD bom = 0xfeff;
        std::vector<unsigned char> buffer(sizeof(bom) + content.length() * sizeof(wchar_t));
        memcpy(buffer.data(), &bom, sizeof(bom));
        if (!content.empty())
        {
            memcpy(buffer.data() + sizeof(bom), content.data(), content.length() * sizeof(wchar_t));
        }
        File::WriteAllBytes(path, buffer.data(), buffer.size());
    }
}

StaticHashGenerator::StaticHashGenerator(const std::wstring& gameDirectory)
    : mGameDirectory(gameDirectory)
{
}

bool StaticHashGenerator::Generate(Result& result, std::wstring& errorMessage)
{
    result = Result{};
    errorMessage.clear();

    std::wstring seed;
    if (!TryReadSeed(this->mGameDirectory, seed) && !TryReadSeedFromExtensionRules(this->mGameDirectory, seed))
    {
        errorMessage = L"未找到 Hash Seed，请先运行字符串Hash提取模块生成 StringHashDumper_Output\\Universal.log，或在扩展集 rules.ini 中配置 HashSeed。";
        return false;
    }

    std::wstring inputDirectory = Path::Combine(this->mGameDirectory, StaticHashInputFolder);
    std::wstring outputDirectory = Path::Combine(this->mGameDirectory, StaticHashOutputFolder);
    std::wstring restoredDirectory = Path::Combine(this->mGameDirectory, RestoredOutputFolder);

    std::set<std::wstring> resourcePaths;
    std::set<std::wstring> directories;
    std::set<std::wstring> fileNames;

    if (!Directory::Exists(inputDirectory))
    {
        TryCopyExtensionStaticInput(this->mGameDirectory);
    }

    if (Directory::Exists(inputDirectory))
    {
        ReadTextLines(Path::Combine(inputDirectory, ResourcePathsFileName), resourcePaths, result.ResourcePathCount);
        ReadTextLines(Path::Combine(inputDirectory, DirectoryNamesFileName), directories, result.DirectoryNameCount);
        ReadTextLines(Path::Combine(inputDirectory, FileNamesFileName), fileNames, result.FileNameCount);
    }

    TryGenerateExtensionStaticInput(this->mGameDirectory, resourcePaths, result.ResourcePathCount);

    if (Directory::Exists(restoredDirectory))
    {
        AddRestoredPaths(restoredDirectory, restoredDirectory, L"", resourcePaths, result.RestoredPathCount);
    }

    for (const auto& path : resourcePaths)
    {
        SplitResourcePath(path, directories, fileNames);
    }

    std::set<std::wstring> normalizedDirectories;
    for (const auto& directory : directories)
    {
        if (directory == EmptyDirectoryMarker)
        {
            normalizedDirectories.insert(L"");
        }
        else
        {
            normalizedDirectories.insert(NormalizeDirectoryHashName(directory));
        }
    }
    directories.swap(normalizedDirectories);

    if (directories.empty() && fileNames.empty())
    {
        Directory::Create(inputDirectory);
        errorMessage = L"没有可计算的候选名。请在 StaticHash_Input 中放入 ResourcePaths.txt、DirectoryNames.txt 或 FileNames.txt。";
        return false;
    }

    Directory::Create(outputDirectory);
    result.OutputDirectory = outputDirectory;
    result.ReportPath = Path::Combine(outputDirectory, StaticHashReportFileName);

    std::wstring directoryLog;
    for (const auto& directory : directories)
    {
        if (IsUnsafeInputPath(directory))
        {
            continue;
        }

        std::wstring displayName = directory.empty() ? EmptyDirectoryMarker : directory;
        directoryLog += displayName + HashLogSplit + DirectoryHash(directory, seed) + L"\r\n";
        ++result.DirectoryHashCount;
    }

    std::wstring fileNameLog;
    for (const auto& fileName : fileNames)
    {
        if (fileName.empty() || IsUnsafeInputPath(fileName))
        {
            continue;
        }

        fileNameLog += fileName + HashLogSplit + FileNameHash(fileName, seed) + L"\r\n";
        ++result.FileNameHashCount;
    }

    AppendUtf16File(Path::Combine(outputDirectory, DirectoryHashFileName), directoryLog);
    AppendUtf16File(Path::Combine(outputDirectory, FileNameHashFileName), fileNameLog);

    std::wstring report;
    report += L"静态Hash映射生成报告\r\n\r\n";
    report += L"输入目录：StaticHash_Input\r\n";
    report += L"输出目录：StaticHash_Output\r\n";
    report += L"Hash Seed：" + seed + L"\r\n\r\n";
    report += L"ResourcePaths.txt 行数：" + std::to_wstring(result.ResourcePathCount) + L"\r\n";
    report += L"DirectoryNames.txt 行数：" + std::to_wstring(result.DirectoryNameCount) + L"\r\n";
    report += L"FileNames.txt 行数：" + std::to_wstring(result.FileNameCount) + L"\r\n";
    report += L"Restored_Extractor_Output 扫描文件数：" + std::to_wstring(result.RestoredPathCount) + L"\r\n\r\n";
    report += L"生成目录Hash数：" + std::to_wstring(result.DirectoryHashCount) + L"\r\n";
    report += L"生成文件名Hash数：" + std::to_wstring(result.FileNameHashCount) + L"\r\n";
    AppendUtf16File(result.ReportPath, report);

    return true;
}
