#pragma once

#include <string>

class StaticHashGeneratorLite
{
public:
    struct Result
    {
        unsigned int resourcePathCount = 0;
        unsigned int directoryHashCount = 0;
        unsigned int fileNameHashCount = 0;
        std::wstring outputDirectory;
    };

    // 轻量静态 Hash 生成器。
    // v0.2 先支持扩展集 rules.ini 的 VoicePattern 规则展开。
    // 后续再补 StaticHash_Input 文本语料和会社集合合并。
    bool GenerateFromExtension(const std::wstring& extensionDirectory,
                               const std::wstring& outputDirectory,
                               Result& result,
                               std::wstring& errorMessage);

    bool GenerateFromBrand(const std::wstring& extensionsRoot,
                           const std::wstring& brand,
                           const std::wstring& outputDirectory,
                           Result& result,
                           std::wstring& errorMessage);
};
