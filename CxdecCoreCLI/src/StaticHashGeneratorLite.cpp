#include "StaticHashGeneratorLite.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    constexpr bool kDebug = false; // 调试日志开关：false 清除所有 debug_xxx.log 输出

    // 扩展集 hash 生成上限
    constexpr int kEstimateCap = 100000;      // 单 range 预估值上限（原3000000，30k时数2上限~48，100k时~162）
    constexpr int kMaxHashEntries = 500000;   // 全局 hash 条目总上限（原3000000，兼顾速度与覆盖率）

    std::wstring g_DebugDir;
    void SetDebugDir(const std::wstring& dir) { g_DebugDir = dir; }

    void WriteDebug(const std::wstring& msg)
    {
        if (!kDebug || g_DebugDir.empty()) return;
        std::wstring path = g_DebugDir + L"\\debug_hash.log";
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        SetFilePointer(h, 0, nullptr, FILE_END);
        // UTF-8 output
        int len = WideCharToMultiByte(CP_UTF8, 0, msg.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (len > 1) {
            std::string utf8(len - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, msg.c_str(), -1, &utf8[0], len, nullptr, nullptr);
            DWORD w; WriteFile(h, utf8.c_str(), (DWORD)utf8.size(), &w, nullptr);
        }
        DWORD w; WriteFile(h, "\r\n", 2, &w, nullptr);
        CloseHandle(h);
    }

    constexpr const wchar_t HashLogSplit[] = L"##YSig##";

    std::wstring Combine(const std::wstring& left, const std::wstring& right)
    {
        if (left.empty()) return right;
        if (right.empty()) return left;
        if (left.back() == L'\\' || left.back() == L'/') return left + right;
        return left + L"\\" + right;
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
        return start == 0 ? value : value.substr(start);
    }

    std::wstring AnsiToWide(const std::string& value)
    {
        if (value.empty()) return L"";
        int length = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
        if (length <= 1)
        {
            length = MultiByteToWideChar(CP_ACP, 0, value.c_str(), -1, nullptr, 0);
            std::wstring output(length ? length - 1 : 0, L'\0');
            if (length > 1) MultiByteToWideChar(CP_ACP, 0, value.c_str(), -1, output.data(), length);
            return output;
        }
        std::wstring output(length - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, output.data(), length);
        return output;
    }

    std::map<std::wstring, std::wstring> ReadIniValues(const std::wstring& path)
    {
        std::ifstream input(path);
        std::map<std::wstring, std::wstring> values;
        std::string raw;
        while (std::getline(input, raw))
        {
            std::wstring line = Trim(AnsiToWide(raw));
            if (line.empty() || line[0] == L';' || line[0] == L'#' || line[0] == L'[') continue;
            size_t equals = line.find(L'=');
            if (equals == std::wstring::npos) continue;
            values[Trim(line.substr(0, equals))] = Trim(line.substr(equals + 1));
        }
        return values;
    }

    std::vector<std::wstring> ReadUtf8Lines(const std::wstring& path)
    {
        std::ifstream input(path);
        std::vector<std::wstring> lines;
        std::string raw;
        while (std::getline(input, raw))
        {
            std::wstring line = Trim(AnsiToWide(raw));
            if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
            lines.push_back(line);
        }
        return lines;
    }

    std::vector<unsigned char> ReadBinaryFile(const std::wstring& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input) return {};
        input.seekg(0, std::ios::end);
        std::streamoff size = input.tellg();
        input.seekg(0, std::ios::beg);
        if (size <= 0) return {};
        std::vector<unsigned char> data((size_t)size);
        input.read(reinterpret_cast<char*>(data.data()), size);
        return data;
    }

    uint32_t ReadU32(const std::vector<unsigned char>& data, size_t& offset)
    {
        if (offset + 4 > data.size()) return 0;
        uint32_t value = (uint32_t)data[offset]
            | ((uint32_t)data[offset + 1] << 8)
            | ((uint32_t)data[offset + 2] << 16)
            | ((uint32_t)data[offset + 3] << 24);
        offset += 4;
        return value;
    }

    bool ReadVarUInt(const std::vector<unsigned char>& data, size_t& offset, uint32_t& value)
    {
        value = 0;
        unsigned int shift = 0;
        while (offset < data.size() && shift < 32)
        {
            unsigned char byte = data[offset++];
            value |= (uint32_t)(byte & 0x7fu) << shift;
            if ((byte & 0x80u) == 0) return true;
            shift += 7;
        }
        return false;
    }

    bool DecompressLzss(const std::vector<unsigned char>& input, std::vector<unsigned char>& output, size_t expectedSize)
    {
        output.clear();
        size_t position = 0;
        while (position < input.size() && output.size() < expectedSize)
        {
            unsigned char flags = input[position++];
            for (unsigned int bit = 0; bit < 8 && position < input.size() && output.size() < expectedSize; ++bit)
            {
                if ((flags & (1u << bit)) != 0)
                {
                    if (position + 2 > input.size()) return false;
                    uint16_t token = (uint16_t)input[position] | ((uint16_t)input[position + 1] << 8);
                    position += 2;
                    size_t offset = (size_t)(token >> 4);
                    size_t length = (size_t)(token & 0x0fu) + 3;
                    if (offset == 0 || offset > output.size()) return false;
                    size_t source = output.size() - offset;
                    for (size_t index = 0; index < length && output.size() < expectedSize; ++index)
                    {
                        output.push_back(output[source + index]);
                    }
                }
                else
                {
                    output.push_back(input[position++]);
                }
            }
        }
        return output.size() == expectedSize;
    }

    std::vector<std::wstring> SplitLinesFromText(const std::wstring& content)
    {
        std::vector<std::wstring> lines;
        size_t start = 0;
        while (start <= content.size())
        {
            size_t end = content.find_first_of(L"\r\n", start);
            std::wstring line = Trim(content.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
            if (!line.empty() && line[0] != L';' && line[0] != L'#') lines.push_back(line);
            if (end == std::wstring::npos) break;
            start = end + 1;
            if (start < content.size() && content[start - 1] == L'\r' && content[start] == L'\n') ++start;
        }
        return lines;
    }

    std::wstring ValueOrEmpty(const std::map<std::wstring, std::wstring>& values, const wchar_t* key)
    {
        auto it = values.find(key);
        return it == values.end() ? L"" : it->second;
    }

    std::vector<std::wstring> SplitList(const std::wstring& text)
    {
        std::vector<std::wstring> output;
        size_t start = 0;
        while (start <= text.size())
        {
            size_t end = text.find_first_of(L",;", start);
            output.push_back(Trim(text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start)));
            if (end == std::wstring::npos) break;
            start = end + 1;
        }
        return output;
    }

    std::wstring ReplaceAll(std::wstring text, const std::wstring& from, const std::wstring& to)
    {
        size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::wstring::npos)
        {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
        return text;
    }

    std::wstring NormalizeResourcePath(std::wstring path)
    {
        std::replace(path.begin(), path.end(), L'/', L'\\');
        while (!path.empty() && (path.front() == L'\\' || path.front() == L'/')) path.erase(path.begin());
        while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
        return path;
    }

    std::wstring NormalizeDirectoryName(std::wstring directory)
    {
        std::replace(directory.begin(), directory.end(), L'\\', L'/');
        while (!directory.empty() && directory.front() == L'/') directory.erase(directory.begin());
        while (!directory.empty() && directory.back() == L'/') directory.pop_back();
        if (!directory.empty()) directory.push_back(L'/');
        return directory;
    }

    bool IsUnsafePath(const std::wstring& path)
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

    void SplitResourcePath(const std::wstring& resourcePath, std::set<std::wstring>& directories, std::set<std::wstring>& fileNames)
    {
        std::wstring path = NormalizeResourcePath(resourcePath);
        if (path.empty() || IsUnsafePath(path)) return;
        size_t split = path.find_last_of(L"\\/");
        if (split == std::wstring::npos)
        {
            directories.insert(L"");
            fileNames.insert(path);
            return;
        }
        std::wstring directory = NormalizeDirectoryName(path.substr(0, split));
        std::wstring fileName = path.substr(split + 1);
        if (!fileName.empty())
        {
            directories.insert(directory);
            fileNames.insert(fileName);
        }
    }

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
        for (unsigned int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (i * 8));
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
        static constexpr uint32_t iv[8] = { 0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au, 0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u };
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
        for (unsigned int i = 0; i < 16; ++i) m[i] = Load32(block + i * 4);
        uint32_t v[16]{};
        for (unsigned int i = 0; i < 8; ++i)
        {
            v[i] = state[i];
            v[i + 8] = iv[i];
        }
        v[12] ^= (uint32_t)counter;
        v[13] ^= (uint32_t)(counter >> 32);
        if (last) v[14] = ~v[14];

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

        for (unsigned int i = 0; i < 8; ++i) state[i] ^= v[i] ^ v[i + 8];
    }

    std::array<uint8_t, 32> Blake2s256(const std::vector<uint8_t>& input)
    {
        uint32_t state[8] = { 0x6A09E667u ^ 0x01010020u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au, 0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u };
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
        if (remaining) memcpy(block, input.data() + offset, remaining);
        counter += remaining;
        Blake2sCompress(state, block, counter, true);

        std::array<uint8_t, 32> digest{};
        for (unsigned int i = 0; i < 8; ++i) Store32(digest.data() + i * 4, state[i]);
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
            for (unsigned int i = 0; i < 8; ++i) m |= ((uint64_t)input[offset + i]) << (8 * i);
            v3 ^= m; SIP_ROUND(); SIP_ROUND(); v0 ^= m;
            offset += 8;
        }

        uint64_t b = ((uint64_t)input.size()) << 56;
        for (unsigned int i = 0; offset + i < input.size(); ++i) b |= ((uint64_t)input[offset + i]) << (8 * i);
        v3 ^= b; SIP_ROUND(); SIP_ROUND(); v0 ^= b;
        v2 ^= 0xff; SIP_ROUND(); SIP_ROUND(); SIP_ROUND(); SIP_ROUND();
#undef SIP_ROUND
        return v0 ^ v1 ^ v2 ^ v3;
    }

    std::vector<uint8_t> MakeHashInput(const std::wstring& text, const std::wstring& seed)
    {
        std::vector<uint8_t> bytes((text.size() + seed.size()) * sizeof(wchar_t));
        if (!text.empty()) memcpy(bytes.data(), text.data(), text.size() * sizeof(wchar_t));
        if (!seed.empty()) memcpy(bytes.data() + text.size() * sizeof(wchar_t), seed.data(), seed.size() * sizeof(wchar_t));
        return bytes;
    }

    std::wstring BytesToHex(const uint8_t* data, size_t size)
    {
        static constexpr wchar_t table[] = L"0123456789ABCDEF";
        std::wstring output;
        output.reserve(size * 2);
        for (size_t i = 0; i < size; ++i)
        {
            output.push_back(table[data[i] >> 4]);
            output.push_back(table[data[i] & 0x0F]);
        }
        return output;
    }

    std::wstring FileNameHash(const std::wstring& fileName, const std::wstring& seed)
    {
        auto digest = Blake2s256(MakeHashInput(fileName, seed));
        return BytesToHex(digest.data(), digest.size());
    }

    std::wstring DirectoryHash(const std::wstring& directoryName, const std::wstring& seed)
    {
        uint8_t output[8]{};
        uint64_t value = SipHash24(MakeHashInput(NormalizeDirectoryName(directoryName), seed));
        Store64(output, value);
        return BytesToHex(output, sizeof(output));
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

    struct RuleSet
    {
        struct PrefixRange
        {
            std::wstring prefix;
            int maxNumber = 0;
        };

        struct PathPattern
        {
            std::wstring pattern;
            std::vector<std::wstring> prefixes;
            std::vector<PrefixRange> prefixRanges;
            std::vector<PrefixRange> prefixRanges2; // per-prefix num2 最大范围
            std::vector<std::wstring> suffixes;
            int maxNumber = 0;
            int numberWidth = 4;
            int maxNumber2 = -1;
            int numberWidth2 = 0;
        };

        std::wstring extensionDirectory;
        std::wstring seed;
        std::vector<PathPattern> patterns;
        std::set<std::wstring> inputResourcePaths;
        std::set<std::wstring> inputDirectoryNames;
        std::set<std::wstring> inputFileNames;
    };

    std::vector<RuleSet::PrefixRange> ParsePrefixRanges(const std::wstring& text)
    {
        std::vector<RuleSet::PrefixRange> output;
        for (const auto& item : SplitList(text))
        {
            size_t split = item.find(L':');
            if (split == std::wstring::npos)
            {
                continue;
            }
            RuleSet::PrefixRange range{};
            range.prefix = Trim(item.substr(0, split));
            range.maxNumber = _wtoi(Trim(item.substr(split + 1)).c_str());
            if (!range.prefix.empty() && range.maxNumber > 0)
            {
                output.push_back(range);
            }
        }
        return output;
    }

    RuleSet::PathPattern PatternFromValues(const std::map<std::wstring, std::wstring>& values)
    {
        RuleSet::PathPattern pattern{};
        pattern.pattern = ValueOrEmpty(values, L"Pattern");
        if (pattern.pattern.empty()) pattern.pattern = ValueOrEmpty(values, L"VoicePattern");
        pattern.prefixes = SplitList(ValueOrEmpty(values, L"Prefixes"));
        pattern.prefixRanges = ParsePrefixRanges(ValueOrEmpty(values, L"PrefixRanges"));
        pattern.prefixRanges2 = ParsePrefixRanges(ValueOrEmpty(values, L"PrefixRanges2"));
        pattern.suffixes = SplitList(ValueOrEmpty(values, L"Suffixes"));
        pattern.maxNumber = _wtoi(ValueOrEmpty(values, L"MaxNumber").c_str());
        pattern.numberWidth = _wtoi(ValueOrEmpty(values, L"NumberWidth").c_str());
        pattern.maxNumber2 = _wtoi(ValueOrEmpty(values, L"MaxNumber2").c_str());
        pattern.numberWidth2 = _wtoi(ValueOrEmpty(values, L"NumberWidth2").c_str());
        if (pattern.numberWidth <= 0) pattern.numberWidth = 4;
        if (pattern.numberWidth2 <= 0) pattern.numberWidth2 = pattern.numberWidth;
        return pattern;
    }

    void AddFrontCodedPathLine(const std::wstring& line, std::wstring& previous, RuleSet& ruleSet)
    {
        size_t split = line.find(L'\t');
        if (split == std::wstring::npos) split = line.find(L'|');
        if (split == std::wstring::npos) return;
        int common = _wtoi(line.substr(0, split).c_str());
        if (common < 0) common = 0;
        if ((size_t)common > previous.size()) common = (int)previous.size();
        std::wstring path = previous.substr(0, (size_t)common) + line.substr(split + 1);
        path = NormalizeResourcePath(path);
        previous = path;
        if (!path.empty() && !IsUnsafePath(path))
        {
            ruleSet.inputResourcePaths.insert(path);
        }
    }

    bool LoadRuleSet(const std::wstring& extensionDirectory, RuleSet& ruleSet, std::wstring& errorMessage)
    {
        std::wstring rulesPath = Combine(extensionDirectory, L"rules.int");
        if (GetFileAttributesW(rulesPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            errorMessage = L"找不到扩展集 rules.int: " + rulesPath;
            return false;
        }

        ruleSet = RuleSet{};
        ruleSet.extensionDirectory = extensionDirectory;
        std::wstring section;
        std::map<std::wstring, std::wstring> metaValues;
        std::map<std::wstring, std::wstring> patternValues;
        std::wstring previousFrontCodedPath;

        auto flushPattern = [&]()
        {
            RuleSet::PathPattern pattern = PatternFromValues(patternValues);
            if (!pattern.pattern.empty())
            {
                ruleSet.patterns.push_back(pattern);
            }
            patternValues.clear();
        };

        std::vector<std::wstring> lines;
        std::vector<unsigned char> binary = ReadBinaryFile(rulesPath);
        bool isV2 = binary.size() >= 22 && memcmp(binary.data(), "CXRI2", 5) == 0;
        bool isV3 = binary.size() >= 30 && memcmp(binary.data(), "CXRI3", 5) == 0;
        if (isV2 || isV3)
        {
            size_t offset = 6;
            uint32_t metaSize = ReadU32(binary, offset);
            uint32_t patternSize = ReadU32(binary, offset);
            uint32_t pathCount = ReadU32(binary, offset);
            uint32_t pathBlockSize = ReadU32(binary, offset);
            uint32_t compression = 0;
            uint32_t storedPathBlockSize = pathBlockSize;
            if (isV3)
            {
                compression = ReadU32(binary, offset);
                storedPathBlockSize = ReadU32(binary, offset);
            }
            if (offset + metaSize + patternSize + storedPathBlockSize <= binary.size())
            {
                std::string metaText(reinterpret_cast<const char*>(binary.data() + offset), metaSize);
                offset += metaSize;
                std::string patternText(reinterpret_cast<const char*>(binary.data() + offset), patternSize);
                offset += patternSize;
                lines = SplitLinesFromText(AnsiToWide(metaText) + L"\n" + AnsiToWide(patternText));
                std::vector<unsigned char> pathData;
                if (compression == 1)
                {
                    std::vector<unsigned char> compressed(binary.begin() + offset, binary.begin() + offset + storedPathBlockSize);
                    if (!DecompressLzss(compressed, pathData, pathBlockSize))
                    {
                        pathData.clear();
                    }
                }
                else
                {
                    pathData.assign(binary.begin() + offset, binary.begin() + offset + storedPathBlockSize);
                }
                std::wstring previous;
                size_t pathOffset = 0;
                size_t pathEnd = pathData.size();
                for (uint32_t index = 0; index < pathCount && pathOffset < pathEnd; ++index)
                {
                    uint32_t common = 0;
                    uint32_t suffixSize = 0;
                    if (!ReadVarUInt(pathData, pathOffset, common) || !ReadVarUInt(pathData, pathOffset, suffixSize) || pathOffset + suffixSize > pathEnd)
                    {
                        break;
                    }
                    if ((size_t)common > previous.size()) common = (uint32_t)previous.size();
                    std::string suffix(reinterpret_cast<const char*>(pathData.data() + pathOffset), suffixSize);
                    pathOffset += suffixSize;
                    std::wstring path = previous.substr(0, common) + AnsiToWide(suffix);
                    previous = path;
                    if (path.rfind(L"|F|", 0) == 0)
                    {
                        std::wstring fileName = NormalizeResourcePath(path.substr(3));
                        if (!fileName.empty() && !IsUnsafePath(fileName))
                        {
                            ruleSet.inputFileNames.insert(fileName);
                        }
                        continue;
                    }
                    path = NormalizeResourcePath(path);
                    if (!path.empty() && !IsUnsafePath(path))
                    {
                        ruleSet.inputResourcePaths.insert(path);
                    }
                }
            }
        }
        else
        {
            lines = ReadUtf8Lines(rulesPath);
        }

        for (const auto& rawLine : lines)
        {
            std::wstring line = Trim(rawLine);
            if (line.empty()) continue;
            if (line.front() == L'[' && line.back() == L']')
            {
                if (section.rfind(L"Pattern", 0) == 0) flushPattern();
                section = line.substr(1, line.size() - 2);
                continue;
            }
            if (section == L"FrontCodedPaths")
            {
                AddFrontCodedPathLine(line, previousFrontCodedPath, ruleSet);
                continue;
            }
            if (section == L"Directories")
            {
                std::wstring directory = NormalizeDirectoryName(line);
                if (!IsUnsafePath(directory))
                {
                    ruleSet.inputDirectoryNames.insert(directory);
                }
                continue;
            }
            size_t equals = line.find(L'=');
            if (equals == std::wstring::npos) continue;
            std::wstring key = Trim(line.substr(0, equals));
            std::wstring value = Trim(line.substr(equals + 1));
            if (section == L"Meta")
            {
                metaValues[key] = value;
            }
            else if (section.rfind(L"Pattern", 0) == 0)
            {
                patternValues[key] = value;
            }
        }
        if (section.rfind(L"Pattern", 0) == 0) flushPattern();

        ruleSet.seed = ValueOrEmpty(metaValues, L"HashSeed");
        if (ruleSet.seed.empty())
        {
            errorMessage = L"rules.int 缺少 HashSeed: " + rulesPath;
            return false;
        }
        return true;
    }

    /// <summary>
    /// 获取指定前缀的 num2 上限。
    /// 优先查 per-prefix PrefixRanges2；没有时用 PrefixRanges 中该前缀的 maxNumber 作为估计；
    /// 连 PrefixRanges 都没有才回退到全局 maxNumber2。
    /// 所有来源均硬限 999，防止单前缀展开超千万级。
    /// </summary>
    int Number2MaxForPrefix(const RuleSet::PathPattern& pattern, const std::wstring& prefix)
    {
        static constexpr int HARD_CAP = 999;
        // 1. 优先 per-prefix num2 范围
        for (const auto& r : pattern.prefixRanges2)
        {
            if (r.prefix == prefix && r.maxNumber >= 0)
                return (std::min)(r.maxNumber, HARD_CAP);
        }
        // 2. 用 PrefixRanges 中同前缀的 maxNumber 作为 num2 估计（旧规则兼容）
        for (const auto& r : pattern.prefixRanges)
        {
            if (r.prefix == prefix && r.maxNumber >= 0)
                return (std::min)(r.maxNumber, HARD_CAP);
        }
        // 3. 回退到全局 maxNumber2（硬限 999 防止内存爆炸）
        return (std::min)(pattern.maxNumber2, HARD_CAP);
    }

    /// <summary>
    /// 展开 Pattern 规则，每生成一条路径就调用 onPath 回调。
    /// onPath 返回 true 继续，false 停止（提前终止）。
    /// 不再将路径缓存在 std::set 中，内存占用极低。
    /// </summary>
    void AddPatternPaths(const RuleSet::PathPattern& pathPattern,
                         const std::function<bool(const std::wstring&)>& onPath)
    {
        WriteDebug(L"[HashGen] AddPatternPaths: ENTER, pattern=" + pathPattern.pattern +
                   L", maxNumber=" + std::to_wstring(pathPattern.maxNumber) +
                   L", numberWidth=" + std::to_wstring(pathPattern.numberWidth) +
                   L", maxNumber2=" + std::to_wstring(pathPattern.maxNumber2) +
                   L", numberWidth2=" + std::to_wstring(pathPattern.numberWidth2) +
                   L", suffixes=" + std::to_wstring((unsigned int)pathPattern.suffixes.size()) +
                   L", prefixes=" + std::to_wstring((unsigned int)pathPattern.prefixes.size()) +
                   L", prefixRanges=" + std::to_wstring((unsigned int)pathPattern.prefixRanges.size()) +
                   L", prefixRanges2=" + std::to_wstring((unsigned int)pathPattern.prefixRanges2.size()));

        if (pathPattern.pattern.empty() || pathPattern.suffixes.empty())
        {
            WriteDebug(L"[HashGen] AddPatternPaths: EXIT early (empty pattern or no suffixes)");
            return;
        }

        auto emitPath = [&](const std::wstring& p) -> bool
        {
            if (!onPath(p)) return false;
            // 为每条生成路径同时生成 .sli 伴侣条目（语音/BGM等的配对描述文件）
            // 此前只在 {num2} 模式生成，但 {num} 单编号模式也需要
            // 注意：游戏中的 .sli 文件名是 xxx.ogg.sli（保留原扩展名再加 .sli），所以 p + L".sli" 是正确行为
            if (!onPath(p + L".sli")) return false;
            return true;
        };

        if (!pathPattern.prefixRanges.empty())
        {
            WriteDebug(L"[HashGen] AddPatternPaths: prefixRanges path, " +
                       std::to_wstring((unsigned int)pathPattern.prefixRanges.size()) + L" ranges");

            // Phase 1: 收集所有前缀的元数据，预计算 number2Max（原 number==0 时的缩减逻辑）
            struct RangeInfo {
                const RuleSet::PrefixRange* range;
                int number2Max;
                int number2Start;
            };
            std::vector<RangeInfo> ranges;
            int globalMaxNumber = 0;
            for (const auto& range : pathPattern.prefixRanges)
            {
                if (range.prefix.empty() || range.maxNumber <= 0) continue;
                int number2Max = Number2MaxForPrefix(pathPattern, range.prefix);
                // 自动缩减：与原来 number==0 时的逻辑相同
                if (number2Max > 0)
                {
                    unsigned int suffixCountForEstimate = 0;
                    for (const auto& s : pathPattern.suffixes)
                        if (s.empty()) ++suffixCountForEstimate;
                    if (suffixCountForEstimate == 0) suffixCountForEstimate = 1;
                    uint64_t estimate = (uint64_t)(range.maxNumber + 1) * (uint64_t)(number2Max + 1) * (uint64_t)suffixCountForEstimate;
                    if (estimate > kEstimateCap)
                    {
                        uint64_t reduced = kEstimateCap / ((uint64_t)(range.maxNumber + 1) * (uint64_t)suffixCountForEstimate);
                        if (reduced < (uint64_t)number2Max)
                            number2Max = (int)reduced;
                    }
                }
                RangeInfo info{ &range, number2Max, number2Max >= 0 ? 0 : -1 };
                ranges.push_back(info);
                if (range.maxNumber > globalMaxNumber)
                    globalMaxNumber = range.maxNumber;
            }

            if (ranges.empty())
            {
                WriteDebug(L"[HashGen] AddPatternPaths: EXIT (no valid ranges)");
                return;
            }

            // Phase 2: 按 num 优先遍历，让所有前缀共享 cap 配额
            // 外循环 = num（所有前缀共享），内循环 = 前缀
            // 这样每个前缀至少覆盖到前 N 个编号，而不是一个前缀吃光 cap
            for (int number = 0; number <= globalMaxNumber; ++number)
            {
                wchar_t numberText[32]{};
                swprintf_s(numberText, L"%0*d", pathPattern.numberWidth, number);
                for (auto& ri : ranges)
                {
                    if (number > ri.range->maxNumber) continue;
                    int number2Max = ri.number2Max;
                    int number2Start = ri.number2Start;
                    for (int number2 = number2Start; number2 <= number2Max; ++number2)
                    {
                        wchar_t numberText2[32]{};
                        if (number2 >= 0) swprintf_s(numberText2, L"%0*d", pathPattern.numberWidth2, number2);
                        for (const auto& suffix : pathPattern.suffixes)
                        {
                            std::wstring path = pathPattern.pattern;
                            path = ReplaceAll(path, L"{prefix}", ri.range->prefix);
                            path = ReplaceAll(path, L"{num}", numberText);
                            path = ReplaceAll(path, L"{num2}", number2 >= 0 ? numberText2 : L"");
                            path = ReplaceAll(path, L"{suffix}", suffix);
                            if (!emitPath(path)) return;
                        }
                    }
                }
            }
            WriteDebug(L"[HashGen] AddPatternPaths: EXIT (prefixRanges path, round-robin)");
            return;
        }

        if (pathPattern.prefixes.empty() || pathPattern.maxNumber <= 0)
        {
            WriteDebug(L"[HashGen] AddPatternPaths: EXIT early (no prefixes or maxNumber<=0)");
            return;
        }

        WriteDebug(L"[HashGen] AddPatternPaths: prefixes path, " +
                   std::to_wstring((unsigned int)pathPattern.prefixes.size()) + L" prefixes");

        // Phase 1: 预计算每个前缀的 number2Max
        struct PrefixInfo {
            std::wstring prefix;
            int number2Max;
            int number2Start;
        };
        std::vector<PrefixInfo> pInfos;
        for (const auto& prefix : pathPattern.prefixes)
        {
            if (prefix.empty()) continue;
            int number2Max = Number2MaxForPrefix(pathPattern, prefix);
            if (number2Max > 0)
            {
                unsigned int suffixCountForEstimate = 0;
                for (const auto& s : pathPattern.suffixes)
                    if (s.empty()) ++suffixCountForEstimate;
                if (suffixCountForEstimate == 0) suffixCountForEstimate = 1;
                uint64_t estimate = (uint64_t)(pathPattern.maxNumber + 1) * (uint64_t)(number2Max + 1) * (uint64_t)suffixCountForEstimate;
                if (estimate > kEstimateCap)
                {
                    uint64_t reduced = kEstimateCap / ((uint64_t)(pathPattern.maxNumber + 1) * (uint64_t)suffixCountForEstimate);
                    if (reduced < (uint64_t)number2Max) number2Max = (int)reduced;
                }
            }
            PrefixInfo pi{ prefix, number2Max, number2Max >= 0 ? 0 : -1 };
            pInfos.push_back(pi);
        }

        if (pInfos.empty())
        {
            WriteDebug(L"[HashGen] AddPatternPaths: EXIT (no valid prefixes)");
            return;
        }

        // Phase 2: 按 num 优先遍历，轮询所有前缀
        for (int number = 0; number <= pathPattern.maxNumber; ++number)
        {
            wchar_t numberText[32]{};
            swprintf_s(numberText, L"%0*d", pathPattern.numberWidth, number);
            for (auto& pi : pInfos)
            {
                int number2Max = pi.number2Max;
                int number2Start = pi.number2Start;
                for (int number2 = number2Start; number2 <= number2Max; ++number2)
                {
                    wchar_t numberText2[32]{};
                    if (number2 >= 0) swprintf_s(numberText2, L"%0*d", pathPattern.numberWidth2, number2);
                    for (const auto& suffix : pathPattern.suffixes)
                    {
                        std::wstring path = pathPattern.pattern;
                        path = ReplaceAll(path, L"{prefix}", pi.prefix);
                        path = ReplaceAll(path, L"{num}", numberText);
                        path = ReplaceAll(path, L"{num2}", number2 >= 0 ? numberText2 : L"");
                        path = ReplaceAll(path, L"{suffix}", suffix);
                        if (!emitPath(path)) return;
                    }
                }
            }
        }
        WriteDebug(L"[HashGen] AddPatternPaths: EXIT (prefixes path, round-robin)");
    }

    void AddRuleSetPaths(const RuleSet& ruleSet,
                         const std::function<bool(const std::wstring&)>& onPath)
    {
        for (const auto& pattern : ruleSet.patterns)
        {
            AddPatternPaths(pattern, onPath);
        }
    }

    bool GenerateFromRuleSets(const std::vector<RuleSet>& ruleSets,
                              const std::wstring& outputDirectory,
                              StaticHashGeneratorLite::Result& result,
                              std::wstring& errorMessage,
                              const std::set<std::wstring>* knownFileNames = nullptr)
    {
        result = StaticHashGeneratorLite::Result{};
        WriteDebug(L"[HashGen] GenerateFromRuleSets: START, ruleSets=" +
                   std::to_wstring((unsigned int)ruleSets.size()) + L", output=" + outputDirectory);

        if (ruleSets.empty())
        {
            errorMessage = L"没有可用扩展集规则。";
            WriteDebug(L"[HashGen] GenerateFromRuleSets: FAIL empty ruleSets");
            return false;
        }

        std::wstring seed = ruleSets.front().seed;
        unsigned int skippedDifferentSeed = 0;

        // 准备输出文件
        WriteDebug(L"[HashGen] GenerateFromRuleSets: EnsureDir...");
        EnsureDirectory(outputDirectory);
        WriteDebug(L"[HashGen] GenerateFromRuleSets: EnsureDir OK");

        std::wstring dirLogPath = Combine(outputDirectory, L"DirectoryHash.log");
        std::wstring fileLogPath = Combine(outputDirectory, L"FileNameHash.log");
        WriteDebug(L"[HashGen] GenerateFromRuleSets: Creating output handles...");
        HANDLE hDirLog = CreateFileW(dirLogPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE hFileLog = CreateFileW(fileLogPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        bool dirOk = (hDirLog != INVALID_HANDLE_VALUE);
        bool fileOk = (hFileLog != INVALID_HANDLE_VALUE);
        WriteDebug(L"[HashGen] GenerateFromRuleSets: Handles dirOk=" + std::wstring(dirOk ? L"yes" : L"no") +
                   L", fileOk=" + std::wstring(fileOk ? L"yes" : L"no"));
        const wchar_t bom = 0xFEFF;
        if (dirOk) { DWORD w; WriteFile(hDirLog, &bom, 2, &w, nullptr); }
        if (fileOk) { DWORD w; WriteFile(hFileLog, &bom, 2, &w, nullptr); }

        // 去重集合（仅用于统计）
        std::set<std::wstring> dirSet, fileSet;
        // 预置已知文件名（来自数据源的 Restored_Extractor_Output）
        // Pattern 展开时会遍历大量不存在的文件名组合，浪费 cap 配额。
        // 预置已知文件名让 Pattern 展开时跳过已存在文件（onPath 的 fileSet.insert 返回 false 时不写入），
        // 同时为每个已知文件名直接写入 hash 条目，不依赖 Pattern 展开。
        if (knownFileNames && fileOk)
        {
            for (const auto& kf : *knownFileNames)
            {
                fileSet.insert(kf);
                std::wstring line = kf + HashLogSplit + FileNameHash(kf, seed) + L"\r\n";
                DWORD w; WriteFile(hFileLog, line.c_str(), (DWORD)(line.size() * 2), &w, nullptr);
                ++result.fileNameHashCount;
            }
        }

        // 路径回调：每一条路径立即拆目录→算 hash→写文件，不缓存
        int callbackCount = 0;
        auto onPath = [&](const std::wstring& rawPath) -> bool
        {
            ++callbackCount;
            // 全局 hash 总条目上限：达到后停止生成，防止 FileNameHash.log 过大
            if ((result.directoryHashCount + result.fileNameHashCount) >= kMaxHashEntries)
                return false;
            std::wstring path = NormalizeResourcePath(rawPath);
            if (path.empty() || IsUnsafePath(path)) return true;
            size_t split = path.find_last_of(L"\\/");
            std::wstring dir = (split == std::wstring::npos) ? L"" : path.substr(0, split);
            std::wstring fname = (split == std::wstring::npos) ? path : path.substr(split + 1);
            if (dirSet.insert(dir).second && dirOk)
            {
                std::wstring line = (dir.empty() ? L"%EmptyString%" : dir) + HashLogSplit + DirectoryHash(dir, seed) + L"\r\n";
                DWORD w; WriteFile(hDirLog, line.c_str(), (DWORD)(line.size() * 2), &w, nullptr);
                ++result.directoryHashCount;
            }
            if (fileSet.insert(fname).second && fileOk)
            {
                std::wstring line = fname + HashLogSplit + FileNameHash(fname, seed) + L"\r\n";
                DWORD w; WriteFile(hFileLog, line.c_str(), (DWORD)(line.size() * 2), &w, nullptr);
                ++result.fileNameHashCount;
            }
            return true;
        };

        // 展开 Pattern 规则 + 扩展集预置路径（全部走 onPath）
        WriteDebug(L"[HashGen] GenerateFromRuleSets: Processing ruleSets (total=" +
                   std::to_wstring((unsigned int)ruleSets.size()) + L")...");
        int rsIndex = 0;
        for (const auto& rs : ruleSets)
        {
            ++rsIndex;
            if (rs.seed != seed)
            {
                ++skippedDifferentSeed;
                WriteDebug(L"[HashGen] GenerateFromRuleSets: RuleSet #" + std::to_wstring(rsIndex) +
                           L" skipped (different seed: " + rs.seed + L" vs " + seed + L")");
                continue;
            }
            WriteDebug(L"[HashGen] GenerateFromRuleSets: RuleSet #" + std::to_wstring(rsIndex) +
                       L" seed=" + seed + L", patterns=" + std::to_wstring((unsigned int)rs.patterns.size()) +
                       L", inputPaths=" + std::to_wstring((unsigned int)rs.inputResourcePaths.size()) +
                       L", inputFiles=" + std::to_wstring((unsigned int)rs.inputFileNames.size()) +
                       L", inputDirs=" + std::to_wstring((unsigned int)rs.inputDirectoryNames.size()));
            callbackCount = 0;
            // 先处理具体路径（inputResourcePaths），确保数据源的确切文件优先写入 hash 表
            // Pattern 展开量巨大（92 prefixes × 613 num × 9999 num2 × 9 suffix × 2 = 千亿级），
            // 会迅速耗尽 cap 导致后续 prefix 和 inputResourcePaths 全被跳过。
            // 必须先写具体路径，再用剩余 cap 做 Pattern 展开。
            int pathCount = 0;
            for (const auto& rp : rs.inputResourcePaths) { if (!onPath(rp)) break; ++pathCount; }
            WriteDebug(L"[HashGen] GenerateFromRuleSets: inputResourcePaths fed=" + std::to_wstring(pathCount) +
                       L", now dirHashes=" + std::to_wstring(result.directoryHashCount) +
                       L", fileHashes=" + std::to_wstring(result.fileNameHashCount));
            WriteDebug(L"[HashGen] GenerateFromRuleSets: Calling AddRuleSetPaths...");
            AddRuleSetPaths(rs, onPath);
            WriteDebug(L"[HashGen] GenerateFromRuleSets: AddRuleSetPaths done, callbacks=" +
                       std::to_wstring(callbackCount) +
                       L", now dirHashes=" + std::to_wstring(result.directoryHashCount) +
                       L", fileHashes=" + std::to_wstring(result.fileNameHashCount));
            for (const auto& fn : rs.inputFileNames) fileSet.insert(fn);
            for (const auto& dn : rs.inputDirectoryNames) dirSet.insert(dn);
        }

        // 补充写入 |F| 文件名
        WriteDebug(L"[HashGen] GenerateFromRuleSets: Supplementary writes (|F| files)...");
        int suppFileCount = 0, suppDirCount = 0;
        for (const auto& rs : ruleSets)
        {
            if (rs.seed != seed) continue;
            for (const auto& fn : rs.inputFileNames)
                if (fileOk) { ++suppFileCount; std::wstring line = fn + HashLogSplit + FileNameHash(fn, seed) + L"\r\n"; DWORD w; WriteFile(hFileLog, line.c_str(), (DWORD)(line.size() * 2), &w, nullptr); ++result.fileNameHashCount; }
            for (const auto& dn : rs.inputDirectoryNames)
                if (dirOk) { ++suppDirCount; std::wstring line = dn + HashLogSplit + DirectoryHash(dn, seed) + L"\r\n"; DWORD w; WriteFile(hDirLog, line.c_str(), (DWORD)(line.size() * 2), &w, nullptr); ++result.directoryHashCount; }
        }
        WriteDebug(L"[HashGen] GenerateFromRuleSets: Supplementary writes done, fileEntries=" +
                   std::to_wstring(suppFileCount) + L", dirEntries=" + std::to_wstring(suppDirCount));

        // 补充写入 .sli 伴侣条目（不受 cap 限制）
        // 从所有 RuleSet 的 inputResourcePaths（FrontCodedPaths）和 inputFileNames（|F|）中
        // 为每个 base 文件名生成 .sli 伴侣 hash。已在 fileSet 中的（来自 Pattern 的 emitPath）跳过。
        int sliCompanionCount = 0;
        if (fileOk)
        {
            for (const auto& rs : ruleSets)
            {
                if (rs.seed != seed) continue;
                // 处理 inputResourcePaths（完整路径→提取文件名→加.sli）
                for (const auto& path : rs.inputResourcePaths)
                {
                    size_t split = path.find_last_of(L"\\/");
                    std::wstring fname = (split == std::wstring::npos) ? path : path.substr(split + 1);
                    std::wstring sliName = fname + L".sli";
                    if (fileSet.find(sliName) == fileSet.end())
                    {
                        fileSet.insert(sliName);
                        std::wstring line = sliName + HashLogSplit + FileNameHash(sliName, seed) + L"\r\n";
                        DWORD w; WriteFile(hFileLog, line.c_str(), (DWORD)(line.size() * 2), &w, nullptr);
                        ++result.fileNameHashCount;
                        ++sliCompanionCount;
                    }
                }
                // 处理 inputFileNames（|F| 条目，直接是文件名→加.sli）
                for (const auto& fn : rs.inputFileNames)
                {
                    std::wstring sliName = fn + L".sli";
                    if (fileSet.find(sliName) == fileSet.end())
                    {
                        fileSet.insert(sliName);
                        std::wstring line = sliName + HashLogSplit + FileNameHash(sliName, seed) + L"\r\n";
                        DWORD w; WriteFile(hFileLog, line.c_str(), (DWORD)(line.size() * 2), &w, nullptr);
                        ++result.fileNameHashCount;
                        ++sliCompanionCount;
                    }
                }
            }
        }
        WriteDebug(L"[HashGen] GenerateFromRuleSets: .sli companion writes done, count=" +
                   std::to_wstring(sliCompanionCount));

        if (dirOk) CloseHandle(hDirLog);
        if (fileOk) CloseHandle(hFileLog);
        WriteDebug(L"[HashGen] GenerateFromRuleSets: Handles closed, final dirHashes=" +
                   std::to_wstring(result.directoryHashCount) + L", fileHashes=" +
                   std::to_wstring(result.fileNameHashCount) + L", uniqueDirs=" +
                   std::to_wstring((unsigned int)dirSet.size()) + L", uniqueFiles=" +
                   std::to_wstring((unsigned int)fileSet.size()));

        if (result.fileNameHashCount == 0 && result.directoryHashCount == 0)
        {
            errorMessage = L"扩展集没有生成任何有效路径。";
            WriteDebug(L"[HashGen] GenerateFromRuleSets: FAIL no hashes generated");
            return false;
        }

        result.resourcePathCount = (unsigned int)fileSet.size();
        result.outputDirectory = outputDirectory;

        std::wstring report;
        report += L"静态 Hash 生成报告\r\n\r\n";
        report += L"HashSeed: " + seed + L"\r\n";
        report += L"参与扩展集: " + std::to_wstring((unsigned int)ruleSets.size() - skippedDifferentSeed) + L"\r\n";
        report += L"跳过不同 HashSeed 扩展集: " + std::to_wstring(skippedDifferentSeed) + L"\r\n";
        report += L"唯一目录数: " + std::to_wstring((unsigned int)dirSet.size()) + L"\r\n";
        report += L"唯一文件名数: " + std::to_wstring((unsigned int)fileSet.size()) + L"\r\n";
        report += L"目录 Hash 总条目: " + std::to_wstring(result.directoryHashCount) + L"\r\n";
        report += L"文件名 Hash 总条目: " + std::to_wstring(result.fileNameHashCount) + L"\r\n";
        WriteUtf16File(Combine(outputDirectory, L"StaticHashReport.txt"), report);
        WriteDebug(L"[HashGen] GenerateFromRuleSets: DONE, report written, paths=" +
                   std::to_wstring(result.resourcePathCount));
        return true;
    }
}

bool StaticHashGeneratorLite::GenerateFromExtension(const std::wstring& extensionDirectory,
                                                    const std::wstring& outputDirectory,
                                                    Result& result,
                                                    std::wstring& errorMessage)
{
    SetDebugDir(extensionDirectory);
    WriteDebug(L"[HashGen] GenerateFromExtension: START, dir=" + extensionDirectory +
               L", output=" + outputDirectory);

    result = Result{};
    errorMessage.clear();

    WriteDebug(L"[HashGen] Calling LoadRuleSet...");
    RuleSet ruleSet;
    if (!LoadRuleSet(extensionDirectory, ruleSet, errorMessage))
    {
        WriteDebug(L"[HashGen] LoadRuleSet FAILED: " + errorMessage);
        return false;
    }
    WriteDebug(L"[HashGen] LoadRuleSet OK: patterns=" + std::to_wstring((unsigned int)ruleSet.patterns.size()) +
               L", inputPaths=" + std::to_wstring((unsigned int)ruleSet.inputResourcePaths.size()) +
               L", inputFiles=" + std::to_wstring((unsigned int)ruleSet.inputFileNames.size()) +
               L", seed=" + ruleSet.seed);

    WriteDebug(L"[HashGen] Calling GenerateFromRuleSets...");
    bool genOk = GenerateFromRuleSets(std::vector<RuleSet>{ ruleSet }, outputDirectory, result, errorMessage,
                                      m_knownFileNames.empty() ? nullptr : &m_knownFileNames);
    WriteDebug(L"[HashGen] GenerateFromRuleSets returned: " + std::wstring(genOk ? L"OK" : L"FAIL") +
               L", paths=" + std::to_wstring(result.resourcePathCount) +
               L", dirHashes=" + std::to_wstring(result.directoryHashCount) +
               L", fileHashes=" + std::to_wstring(result.fileNameHashCount) +
               (genOk ? L"" : L", error=" + errorMessage));
    return genOk;
}

bool StaticHashGeneratorLite::GenerateFromBrand(const std::wstring& extensionsRoot,
                                                const std::wstring& brand,
                                                const std::wstring& outputDirectory,
                                                Result& result,
                                                std::wstring& errorMessage)
{
    result = Result{};
    errorMessage.clear();

    std::wstring brandDirectory = Combine(extensionsRoot, brand);
    if (!DirectoryExists(brandDirectory))
    {
        errorMessage = L"找不到会社扩展集目录: " + brandDirectory;
        return false;
    }

    std::vector<RuleSet> ruleSets;
    std::wstring pattern = Combine(brandDirectory, L"*");
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
    {
        errorMessage = L"会社扩展集目录为空: " + brandDirectory;
        return false;
    }

    do
    {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
        RuleSet ruleSet;
        std::wstring child = Combine(brandDirectory, data.cFileName);
        std::wstring childError;
        if (LoadRuleSet(child, ruleSet, childError))
        {
            ruleSets.push_back(ruleSet);
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);

    return GenerateFromRuleSets(ruleSets, outputDirectory, result, errorMessage);
}

std::wstring StaticHashGeneratorLite::ComputeFileHash(const std::wstring& fileName, const std::wstring& seed) {
    return FileNameHash(fileName, seed);
}

std::wstring StaticHashGeneratorLite::ComputeDirHash(const std::wstring& dirName, const std::wstring& seed) {
    return DirectoryHash(dirName, seed);
}