#include "HashTailer.h"
#include <windows.h>
#include <vector>
#include <sstream>

// ========== 编码检测常量 ==========

static constexpr uint16_t BOM_UTF16LE = 0xFFFE;
static constexpr uint16_t BOM_UTF16BE = 0xFEFF;
static constexpr uint16_t BOM_UTF8 = 0xBBEF; // EF BB 的前两个字节

HashTailer::HashTailer(const std::wstring& filePath)
    : path_(filePath)
{
}

bool HashTailer::Exists() const
{
    DWORD attr = GetFileAttributesW(path_.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

/// <summary>
/// 检测文件编码（通过 BOM），仅在 offset 为 0 时有效
/// </summary>
static FileEncoding DetectEncoding(HANDLE hFile)
{
    unsigned char bom[4] = {};
    DWORD read = 0;
    SetFilePointer(hFile, 0, nullptr, FILE_BEGIN);
    if (!ReadFile(hFile, bom, 4, &read, nullptr) || read < 2)
        return FileEncoding::UTF8;

    if (bom[0] == 0xFF && bom[1] == 0xFE)
        return FileEncoding::UTF16LE;
    if (bom[0] == 0xFE && bom[1] == 0xFF)
        return FileEncoding::UTF16BE;
    if (bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF)
        return FileEncoding::UTF8;
    return FileEncoding::UTF8; // 无 BOM，当作 UTF-8
}

std::vector<std::string> HashTailer::ReadNewLines()
{
    std::vector<std::string> lines;
    if (!Exists())
        return lines;

    HANDLE hFile = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return lines;

    LARGE_INTEGER liSize{};
    GetFileSizeEx(hFile, &liSize);
    uint64_t fileSize = (uint64_t)liSize.QuadPart;

    // 检测编码（仅在首次读取时，offset=0 时做）
    FileEncoding encoding = FileEncoding::UTF8;
    uint64_t bomSize = 0;
    if (offset_ == 0)
    {
        encoding = DetectEncoding(hFile);
        if (encoding == FileEncoding::UTF16LE) bomSize = 2;
        else if (encoding == FileEncoding::UTF16BE) bomSize = 2;
        else if (encoding == FileEncoding::UTF8)
        {
            unsigned char bom3[3];
            DWORD read3 = 0;
            SetFilePointer(hFile, 0, nullptr, FILE_BEGIN);
            if (ReadFile(hFile, bom3, 3, &read3, nullptr) && read3 == 3 &&
                bom3[0] == 0xEF && bom3[1] == 0xBB && bom3[2] == 0xBF)
                bomSize = 3;
        }
    }

    // 文件被重建/清空
    if (fileSize < offset_)
        offset_ = 0;

    if (fileSize <= (offset_ + bomSize))
    {
        CloseHandle(hFile);
        return lines;
    }

    // 确保 offset 至少跳过 BOM
    if (offset_ < bomSize)
        offset_ = bomSize;

    // 计算要读取的字节数
    uint64_t toRead = fileSize - offset_;
    if (toRead > MAX_READ_SIZE)
        toRead = MAX_READ_SIZE;

    // 读取原始字节
    std::vector<char> rawBuf((size_t)toRead);
    LARGE_INTEGER liOff{};
    liOff.QuadPart = (LONGLONG)offset_;
    SetFilePointerEx(hFile, liOff, nullptr, FILE_BEGIN);

    DWORD bytesRead = 0;
    if (!ReadFile(hFile, rawBuf.data(), (DWORD)toRead, &bytesRead, nullptr) || bytesRead == 0)
    {
        CloseHandle(hFile);
        return lines;
    }
    offset_ += bytesRead;

    // 检查是否以 \n 结尾——如果没有，说明最后一行不完整（dumper 还在写）
    // 需要抛弃不完整行并回退 offset，避免显示乱码
    {
        bool endsWithNewline = false;
        if (encoding == FileEncoding::UTF16LE)
        {
            // UTF-16LE：检查末尾是否为 \n\0
            if (bytesRead >= 2 && rawBuf[bytesRead - 2] == '\n' && rawBuf[bytesRead - 1] == '\0')
                endsWithNewline = true;
        }
        else
        {
            // UTF-8：检查末尾是否为 \n
            if (bytesRead > 0 && rawBuf[bytesRead - 1] == '\n')
                endsWithNewline = true;
        }

        if (!endsWithNewline && bytesRead > 0)
        {
            // 往回找到最近的 \n，截断到那里
            int truncatePos = -1;
            if (encoding == FileEncoding::UTF16LE)
            {
                // UTF-16LE：每 2 字节一个字符
                for (int i = ((int)bytesRead - 2) & ~1; i >= 0; i -= 2)
                {
                    if (rawBuf[i] == '\n' && rawBuf[i + 1] == '\0')
                    {
                        truncatePos = i + 2;
                        break;
                    }
                }
            }
            else
            {
                // UTF-8：逐字节查找
                for (int i = (int)bytesRead - 1; i >= 0; i--)
                {
                    if (rawBuf[i] == '\n')
                    {
                        truncatePos = i + 1;
                        break;
                    }
                }
            }

            if (truncatePos > 0 && truncatePos < (int)bytesRead)
            {
                uint64_t incompleteBytes = bytesRead - truncatePos;
                offset_ -= incompleteBytes;
                bytesRead = (DWORD)truncatePos;
                rawBuf.resize(bytesRead);
            }
        }
    }

    // 按编码转换 → UTF-8 字符串
    std::string utf8Text;
    if (encoding == FileEncoding::UTF16LE)
    {
        // UTF-16LE → 宽字符串 → UTF-8
        int wlen = bytesRead / 2;
        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, (LPCWCH)rawBuf.data(), wlen, nullptr, 0, nullptr, nullptr);
        if (utf8Len > 0)
        {
            utf8Text.resize(utf8Len);
            WideCharToMultiByte(CP_UTF8, 0, (LPCWCH)rawBuf.data(), wlen,
                                &utf8Text[0], utf8Len, nullptr, nullptr);
        }
    }
    else
    {
        // UTF-8 或 UTF-16BE（第一版不处理 UTF-16BE）
        utf8Text.assign(rawBuf.data(), bytesRead);
    }

    // 按行拆分 UTF-8 文本
    if (!utf8Text.empty())
    {
        // 处理行末可能被截断的情况：末尾不完整行不要
        // 先试着找最后一个 \n，如果末尾没有 \n 则整块是完整内容
        std::istringstream stream(utf8Text);
        std::string line;
        while (std::getline(stream, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (!line.empty())
                lines.push_back(line);
        }

        // istringstream 的 getline 会吃掉 \n 但保留空行
        // 如果末尾内容是完整的，最后一行之后不会有额外的空行问题
    }

    CloseHandle(hFile);
    return lines;
}

void HashTailer::SkipToEnd()
{
    if (!Exists())
        return;

    HANDLE hFile = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return;

    LARGE_INTEGER liSize{};
    GetFileSizeEx(hFile, &liSize);
    offset_ = (uint64_t)liSize.QuadPart;
    CloseHandle(hFile);
}

void HashTailer::ResetOffset()
{
    offset_ = 0;
}
