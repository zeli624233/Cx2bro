#pragma once
#include <string>
#include <unordered_set>

/// <summary>
/// Hash 去重统计器
/// 支持多种日志行格式，维护唯一 Hash 集合
/// </summary>
class HashStats
{
public:
    /// <summary>
    /// 添加一行 DirectoryHash.log 内容
    /// </summary>
    void AddDirectoryLine(const std::string& line);

    /// <summary>
    /// 添加一行 FileNameHash.log 内容
    /// </summary>
    void AddFileNameLine(const std::string& line);

    /// <summary>
    /// 获取唯一 DirectoryHash 数量
    /// </summary>
    size_t DirectoryCount() const { return directoryHashes_.size(); }

    /// <summary>
    /// 获取唯一 FileNameHash 数量
    /// </summary>
    size_t FileNameCount() const { return fileNameHashes_.size(); }

    /// <summary>
    /// 清空所有集合
    /// </summary>
    void Clear();

    /// <summary>
    /// 从已有集合导入（用于继续上次 session）
    /// </summary>
    void ImportDirectorySet(const std::unordered_set<std::string>& dirSet);
    void ImportFileNameSet(const std::unordered_set<std::string>& fileSet);

    /// <summary>
    /// 获取内部集合（用于持久化保存）
    /// </summary>
    const std::unordered_set<std::string>& DirectorySet() const { return directoryHashes_; }
    const std::unordered_set<std::string>& FileNameSet() const { return fileNameHashes_; }

private:
    std::unordered_set<std::string> directoryHashes_;
    std::unordered_set<std::string> fileNameHashes_;

    /// <summary>
    /// 从行中提取规范化后的唯一 key
    /// </summary>
    static std::string NormalizeLine(const std::string& line);
};
