#include <windows.h>
#include <detours.h>
#include <string>

#include "path.h"
#include "encoding.h"
#include "ExtractorOutputRestorer.h"
#include "StaticHashGenerator.h"

#pragma comment(linker, "/MERGE:\".detourd=.data\"")
#pragma comment(linker, "/MERGE:\".detourc=.rdata\"")

namespace
{
    enum BackendCommand
    {
        CommandExtract = 1,
        CommandStaticHash = 2,
        CommandRestore = 3,
        CommandStringHash = 4,
        CommandKeyDump = 5
    };

    void CopyMessage(wchar_t* buffer, int bufferLength, const std::wstring& message)
    {
        if (!buffer || bufferLength <= 0)
        {
            return;
        }

        lstrcpynW(buffer, message.c_str(), bufferLength);
    }

    bool RunRestore(const std::wstring& gameDirectory, std::wstring& message)
    {
        ExtractorOutputRestorer restorer(gameDirectory);
        ExtractorOutputRestorer::Result result{};
        std::wstring errorMessage;
        if (!restorer.Restore(result, errorMessage))
        {
            message = errorMessage.empty() ? L"Restore failed." : errorMessage;
            return false;
        }

        message = L"Restore completed. ";
        message += std::to_wstring(result.RestoredFiles);
        message += L"/";
        message += std::to_wstring(result.TotalFiles);
        return true;
    }

    bool RunStaticHash(const std::wstring& gameDirectory, std::wstring& message)
    {
        StaticHashGenerator generator(gameDirectory);
        StaticHashGenerator::Result result{};
        std::wstring errorMessage;
        if (!generator.Generate(result, errorMessage))
        {
            message = errorMessage.empty() ? L"Static hash generation failed." : errorMessage;
            return false;
        }

        message = L"Static hash completed. Directory hashes: ";
        message += std::to_wstring(result.DirectoryHashCount);
        message += L", file hashes: ";
        message += std::to_wstring(result.FileNameHashCount);
        return true;
    }

    bool RunInject(const std::wstring& moduleDirectory,
                   const std::wstring& gameExePath,
                   const std::wstring& gameDirectory,
                   const wchar_t* moduleName,
                   bool waitForProcess,
                   std::wstring& message)
    {
        std::wstring injectDllFullPath = Path::Combine(moduleDirectory, moduleName);
        if (!Path::Exists(injectDllFullPath))
        {
            message = L"Module not found: ";
            message += injectDllFullPath;
            return false;
        }

        std::string injectDllFullPathA = Encoding::UnicodeToAnsi(injectDllFullPath, Encoding::CodePage::ACP);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};

        if (!DetourCreateProcessWithDllW(gameExePath.c_str(),
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         FALSE,
                                         0u,
                                         nullptr,
                                         gameDirectory.c_str(),
                                         &si,
                                         &pi,
                                         injectDllFullPathA.c_str(),
                                         nullptr))
        {
            message = L"Create process failed.";
            return false;
        }

        if (waitForProcess)
        {
            WaitForSingleObject(pi.hProcess, INFINITE);
        }

        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        message = L"Process started with ";
        message += moduleName;
        return true;
    }
}

extern "C" __declspec(dllexport) int __stdcall CxdecRunCommand(int command,
                                                               const wchar_t* moduleDirectory,
                                                               const wchar_t* gameExePath,
                                                               wchar_t* messageBuffer,
                                                               int messageBufferLength)
{
    if (!moduleDirectory || !gameExePath || !gameExePath[0])
    {
        CopyMessage(messageBuffer, messageBufferLength, L"Invalid argument.");
        return 2;
    }

    std::wstring message;
    std::wstring gameExe(gameExePath);
    std::wstring gameDirectory = Path::GetDirectoryName(gameExe);
    bool success = false;

    switch (command)
    {
        case CommandExtract:
            success = RunInject(moduleDirectory, gameExe, gameDirectory, L"CxdecExtractorUI.dll", false, message);
            break;
        case CommandStaticHash:
            success = RunStaticHash(gameDirectory, message);
            break;
        case CommandRestore:
            success = RunRestore(gameDirectory, message);
            break;
        case CommandStringHash:
            success = RunInject(moduleDirectory, gameExe, gameDirectory, L"CxdecStringDumper.dll", false, message);
            break;
        case CommandKeyDump:
            success = RunInject(moduleDirectory, gameExe, gameDirectory, L"CxdecKeyDumper.dll", true, message);
            break;
        default:
            message = L"Unknown command.";
            success = false;
            break;
    }

    CopyMessage(messageBuffer, messageBufferLength, message);
    return success ? 0 : 1;
}
