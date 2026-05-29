#pragma once
#include <string>
#include <vector>
#include <cstdint>

/// <summary>
/// 文件编码
/// </summary>
enum class FileEncoding
{
    UTF8,
    UTF16LE,
    UTF16BE,
};

/// <summary>
/// 文件增量读取器（tail 模式）
/// 维护文件 offset，每次只读取新增部分
/// 支持 UTF-8 和 UTF-16LE 编码（自动检测 BOM）
/// </summary>
class HashTailer
{
public:
    explicit HashTailer(const std::wstring& filePath);

    /// <summary>
    /// 文件是否存在
    /// </summary>
    bool Exists() const;

    /// <summary>
    /// 读取新增行（从上次 offset 到当前文件末尾，最多 maxReadSize 字节）
    /// 返回 UTF-8 编码的行
    /// maxReadSize=0 时不限制读取大小（全量读取）
    /// </summary>
    std::vector<std::string> ReadNewLines(uint64_t maxReadSize = MAX_READ_SIZE);

    /// <summary>
    /// 获取当前文件大小（字节）
    /// </summary>
    uint64_t FileSize() const;

    /// <summary>
    /// 检查文件是否被截断/替换（当前文件 < offset_）
    /// </summary>
    bool WasTruncated() const;

    /// <summary>
    /// 重置 offset 到文件末尾（跳过已有内容）
    /// </summary>
    void SkipToEnd();

    /// <summary>
    /// 重置 offset 到文件开头
    /// </summary>
    void ResetOffset();

    /// <summary>
    /// 获取当前 offset（原始字节位置）
    /// </summary>
    uint64_t Offset() const { return offset_; }

    /// <summary>
    /// 设置 offset
    /// </summary>
    void SetOffset(uint64_t offset) { offset_ = offset; }

    /// <summary>
    /// 获取文件路径
    /// </summary>
    const std::wstring& Path() const { return path_; }

public:
    static constexpr uint64_t MAX_READ_SIZE = 8 * 1024 * 1024; // 单次最大 8MB（速度加倍）

private:
    std::wstring path_;
    uint64_t offset_ = 0;
    bool encodingDetected_ = false;   // 是否已检测过编码
    FileEncoding encoding_ = FileEncoding::UTF8;
    uint64_t bomSize_ = 0;           // BOM 字节数
};
