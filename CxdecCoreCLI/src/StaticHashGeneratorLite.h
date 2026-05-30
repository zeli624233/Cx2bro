#pragma once

#include <set>
#include <string>

class StaticHashGeneratorLite
{
public:
    StaticHashGeneratorLite() = default;

    struct Result
    {
        unsigned int resourcePathCount = 0;
        unsigned int directoryHashCount = 0;
        unsigned int fileNameHashCount = 0;
        std::wstring outputDirectory;
    };

    // 预置已知文件名（来自数据源的 Restored_Extractor_Output）
    // 在 Pattern 展开前被加入去重集合，避免浪费 cap 在已有文件名上。
    void SetKnownFileNames(const std::set<std::wstring>& names) { m_knownFileNames = names; }

    bool GenerateFromExtension(const std::wstring& extensionDirectory,
                               const std::wstring& outputDirectory,
                               Result& result,
                               std::wstring& errorMessage);

    bool GenerateFromBrand(const std::wstring& extensionsRoot,
                           const std::wstring& brand,
                           const std::wstring& outputDirectory,
                           Result& result,
                           std::wstring& errorMessage);

    static std::wstring ComputeFileHash(const std::wstring& fileName, const std::wstring& seed);
    static std::wstring ComputeDirHash(const std::wstring& dirName, const std::wstring& seed);

private:
    std::set<std::wstring> m_knownFileNames;
};
