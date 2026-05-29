#pragma once

#include <string>

class PublisherExtensionBuilderLite
{
public:
    struct Result
    {
        unsigned int resourcePathCount = 0;
        unsigned int directoryNameCount = 0;
        unsigned int fileNameCount = 0;
        bool minimalPackage = false;
        bool usedStaticInput = false;
        std::wstring sourceRestoredDirectory;
        std::wstring draftDirectory;
        std::wstring reportPath;
    };

    // 从已还原的资源目录生成扩展集草稿。
    // 这一步只整理候选名和基础配置，不直接覆盖正式 Extensions 目录。
    bool BuildFromGameDirectory(const std::wstring& gameExePath,
                                const std::wstring& draftDirectory,
                                const std::wstring& brand,
                                Result& result,
                                std::wstring& errorMessage) const;
};
