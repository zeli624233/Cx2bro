#include "HashStats.h"
#include <algorithm>
#include <cctype>

std::string HashStats::NormalizeLine(const std::string& line)
{
    if (line.empty())
        return "";

    // 去掉首尾空白
    size_t start = 0;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
        ++start;
    size_t end = line.size();
    while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\t'))
        --end;

    if (start >= end)
        return "";

    std::string trimmed = line.substr(start, end - start);

    // 注释行跳过
    if (trimmed[0] == ';' || trimmed[0] == '#' || trimmed[0] == '/')
        return "";

    // 尝试多种格式提取唯一 key
    // 格式1: "hash=plaintext" 或 "plaintext=hash"
    // 格式2: "hash -> plaintext" 或 "plaintext -> hash"
    // 格式3: "hash plaintext"
    // 格式4: "plaintext##YSig##hash" (标准格式)

    // 优先匹配 "##YSig##" 分隔符（标准格式）
    {
        auto pos = trimmed.find("##YSig##");
        if (pos != std::string::npos)
        {
            // 取右边 hash 部分作为唯一 key
            std::string right = trimmed.substr(pos + 9);
            // 去掉首尾空白
            size_t rStart = 0;
            while (rStart < right.size() && (right[rStart] == ' ' || right[rStart] == '\t'))
                ++rStart;
            size_t rEnd = right.size();
            while (rEnd > rStart && (right[rEnd - 1] == ' ' || right[rEnd - 1] == '\t'))
                --rEnd;
            if (rStart < rEnd)
                return right.substr(rStart, rEnd - rStart);
        }
    }

    // 尝试 " -> " 分隔符
    {
        auto pos = trimmed.find(" -> ");
        if (pos != std::string::npos)
        {
            std::string right = trimmed.substr(pos + 4);
            size_t rStart = 0;
            while (rStart < right.size() && (right[rStart] == ' ' || right[rStart] == '\t'))
                ++rStart;
            size_t rEnd = right.size();
            while (rEnd > rStart && (right[rEnd - 1] == ' ' || right[rEnd - 1] == '\t'))
                --rEnd;
            if (rStart < rEnd)
                return right.substr(rStart, rEnd - rStart);
        }
    }

    // 尝试 "=" 分隔符
    {
        auto pos = trimmed.find('=');
        if (pos != std::string::npos)
        {
            std::string right = trimmed.substr(pos + 1);
            size_t rStart = 0;
            while (rStart < right.size() && (right[rStart] == ' ' || right[rStart] == '\t'))
                ++rStart;
            size_t rEnd = right.size();
            while (rEnd > rStart && (right[rEnd - 1] == ' ' || right[rEnd - 1] == '\t'))
                --rEnd;
            if (rStart < rEnd)
                return right.substr(rStart, rEnd - rStart);
        }
    }

    // 兜底：用整行做 key
    return trimmed;
}

void HashStats::AddDirectoryLine(const std::string& line)
{
    std::string key = NormalizeLine(line);
    if (!key.empty())
        directoryHashes_.insert(key);
}

void HashStats::AddFileNameLine(const std::string& line)
{
    std::string key = NormalizeLine(line);
    if (!key.empty())
        fileNameHashes_.insert(key);
}

void HashStats::Clear()
{
    directoryHashes_.clear();
    fileNameHashes_.clear();
}

void HashStats::ImportDirectorySet(const std::unordered_set<std::string>& dirSet)
{
    directoryHashes_.insert(dirSet.begin(), dirSet.end());
}

void HashStats::ImportFileNameSet(const std::unordered_set<std::string>& fileSet)
{
    fileNameHashes_.insert(fileSet.begin(), fileSet.end());
}
