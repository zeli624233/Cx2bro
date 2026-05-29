#pragma once

#include <string>
#include <vector>

class ExtractorOutputRestorer
{
public:
    struct DirectoryResult
    {
        std::wstring Name;
        unsigned int TotalFiles = 0;
        unsigned int RestoredFiles = 0;
        unsigned int MissingDirectoryHash = 0;
        unsigned int MissingFileNameHash = 0;
        unsigned int CopyFailed = 0;
    };

    struct Result
    {
        unsigned int TotalFiles = 0;
        unsigned int RestoredFiles = 0;
        unsigned int MissingDirectoryHash = 0;
        unsigned int MissingFileNameHash = 0;
        unsigned int CopyFailed = 0;
        std::wstring ReportPath;
        std::vector<DirectoryResult> Directories;
    };

    explicit ExtractorOutputRestorer(const std::wstring& gameDirectory);

    bool Restore(Result& result, std::wstring& errorMessage);

private:
    std::wstring mGameDirectory;
};
