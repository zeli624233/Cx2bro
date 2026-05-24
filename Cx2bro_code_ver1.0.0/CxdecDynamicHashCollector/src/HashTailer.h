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
    /// 读取新增行（从上次 offset 到当前文件末尾）
    /// 返回 UTF-8 编码的行
    /// </summary>
    std::vector<std::string> ReadNewLines();

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

private:
    std::wstring path_;
    uint64_t offset_ = 0;

    static constexpr uint64_t MAX_READ_SIZE = 4 * 1024 * 1024; // 单次最大 4MB
};
