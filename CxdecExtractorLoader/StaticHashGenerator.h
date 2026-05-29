#pragma once

#include <string>

class StaticHashGenerator
{
public:
    struct Result
    {
        unsigned int ResourcePathCount = 0;
        unsigned int DirectoryNameCount = 0;
        unsigned int FileNameCount = 0;
        unsigned int RestoredPathCount = 0;
        unsigned int DirectoryHashCount = 0;
        unsigned int FileNameHashCount = 0;
        std::wstring OutputDirectory;
        std::wstring ReportPath;
    };

    explicit StaticHashGenerator(const std::wstring& gameDirectory);

    bool Generate(Result& result, std::wstring& errorMessage);

private:
    std::wstring mGameDirectory;
};
