#include <windows.h>

#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "ResourceRestorerLite.h"
#include "RestoreUiLite.h"
#include "PublisherTestUiLite.h"
#include "StaticHashGeneratorLite.h"
#include "DynamicLauncherLite.h"
#include "PublisherExtensionBuilderLite.h"

namespace
{
    constexpr const wchar_t AppVersion[] = L"v1.0.0";

    struct Options
    {
        std::wstring mode;
        std::wstring game;
        std::wstring extension;
        std::wstring extensionsRoot;
        std::wstring brand;
        std::wstring outputRoot;
        std::wstring workspace;
        std::wstring packageListFile;
        int workerCount = 1;
        bool notifyPopup = true;
        bool notifySound = true;
        bool restore = false;
    };

    std::wstring Utf8ToWide(const char* value)
    {
        if (!value)
        {
            return L"";
        }

        int length = MultiByteToWideChar(CP_UTF8, 0, value, -1, nullptr, 0);
        if (length <= 1)
        {
            length = MultiByteToWideChar(CP_ACP, 0, value, -1, nullptr, 0);
            std::wstring output(length ? length - 1 : 0, L'\0');
            if (length > 1)
            {
                MultiByteToWideChar(CP_ACP, 0, value, -1, output.data(), length);
            }
            return output;
        }

        std::wstring output(length - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value, -1, output.data(), length);
        return output;
    }

    std::string WideToUtf8(const std::wstring& value)
    {
        if (value.empty())
        {
            return "";
        }

        int length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string output(length ? length - 1 : 0, '\0');
        if (length > 1)
        {
            WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, output.data(), length, nullptr, nullptr);
        }
        return output;
    }

    void PrintLine(const std::wstring& text)
    {
        std::cout << WideToUtf8(text) << "\n";
    }

    std::wstring Combine(const std::wstring& left, const std::wstring& right)
    {
        if (left.empty())
        {
            return right;
        }

        if (right.empty())
        {
            return left;
        }

        if (left.back() == L'\\' || left.back() == L'/')
        {
            return left + right;
        }

        return left + L"\\" + right;
    }

    std::wstring DirectoryName(const std::wstring& path)
    {
        size_t pos = path.find_last_of(L"\\/");
        return pos == std::wstring::npos ? L"" : path.substr(0, pos);
    }

    std::wstring AppDirectory()
    {
        wchar_t path[MAX_PATH]{};
        DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        return DirectoryName(std::wstring(path, length));
    }

    bool DirectoryExists(const std::wstring& path)
    {
        DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    void EnsureDirectory(const std::wstring& path)
    {
        if (path.empty() || DirectoryExists(path))
        {
            return;
        }

        // Win32 只能逐级创建目录，所以这里递归创建父目录。
        size_t split = path.find_last_of(L"\\/");
        if (split != std::wstring::npos)
        {
            EnsureDirectory(path.substr(0, split));
        }

        CreateDirectoryW(path.c_str(), nullptr);
    }

    bool ParseArgs(int argc, wchar_t** argv, Options& options)
    {
        for (int i = 1; i < argc; ++i)
        {
            std::wstring key = argv[i];
            if (key == L"--mode" && i + 1 < argc)
            {
                options.mode = argv[++i];
            }
            else if (key == L"--game" && i + 1 < argc)
            {
                options.game = argv[++i];
            }
            else if (key == L"--extension" && i + 1 < argc)
            {
                options.extension = argv[++i];
            }
            else if (key == L"--extensions-root" && i + 1 < argc)
            {
                options.extensionsRoot = argv[++i];
            }
            else if (key == L"--brand" && i + 1 < argc)
            {
                options.brand = argv[++i];
            }
            else if (key == L"--output-root" && i + 1 < argc)
            {
                options.outputRoot = argv[++i];
            }
            else if (key == L"--workspace" && i + 1 < argc)
            {
                options.workspace = argv[++i];
            }
            else if (key == L"--package-list" && i + 1 < argc)
            {
                options.packageListFile = argv[++i];
            }
            else if (key == L"--workers" && i + 1 < argc)
            {
                options.workerCount = _wtoi(argv[++i]);
            }
            else if (key == L"--notify-popup" && i + 1 < argc)
            {
                options.notifyPopup = _wtoi(argv[++i]) != 0;
            }
            else if (key == L"--notify-sound" && i + 1 < argc)
            {
                options.notifySound = _wtoi(argv[++i]) != 0;
            }
            else if (key == L"--restore")
            {
                options.restore = true;
            }
            else if (key == L"--help" || key == L"-h")
            {
                options.mode = L"help";
            }
        }

        return !options.mode.empty();
    }

    void PrintHelp()
    {
        PrintLine(std::wstring(L"CxdecCoreCLI ") + AppVersion);
        PrintLine(L"");
        PrintLine(L"Usage:");
        PrintLine(L"  CxdecCoreCLI.exe --version");
        PrintLine(L"  CxdecCoreCLI.exe --mode user1 --game <game.exe> --extension <extension_dir>");
        PrintLine(L"  CxdecCoreCLI.exe --mode user2 --game <game.exe> --brand <brand> --extensions-root <Extensions>");
        PrintLine(L"  CxdecCoreCLI.exe --mode user3 --game <game.exe>");
        PrintLine(L"  CxdecCoreCLI.exe --mode dynamic-extract --game <game.exe>");
        PrintLine(L"  CxdecCoreCLI.exe --mode batch-extract-xp3 --game <game.exe> --output-root <User\\N> --package-list <tasks.txt>");
        PrintLine(L"  CxdecCoreCLI.exe --mode dynamic-stringhash --game <game.exe>");
        PrintLine(L"  CxdecCoreCLI.exe --mode dynamic-keydump --game <game.exe>");
        PrintLine(L"  CxdecCoreCLI.exe --mode restore-ui --workspace <User\\N> --workers <n>");
        PrintLine(L"  CxdecCoreCLI.exe --mode publisher-test-ui --output-root <User\\N> --extension <extension_dir>");
        PrintLine(L"  CxdecCoreCLI.exe --mode publisher-make --game <game.exe>");
        PrintLine(L"");
        PrintLine(L"Options:");
        PrintLine(L"  --restore    Generate Restored_Extractor_Output after hash preparation");
    }

    int PrepareUser(const Options& options, int mode)
    {
        if (options.game.empty())
        {
            PrintLine(L"ERROR: missing --game");
            return 2;
        }

        std::wstring gameDirectory = DirectoryName(options.game);
        std::wstring root = options.outputRoot.empty()
            ? Combine(Combine(gameDirectory, L"User"), std::to_wstring(mode))
            : options.outputRoot;
        EnsureDirectory(Combine(root, L"Extractor_Output"));
        EnsureDirectory(Combine(root, L"StaticHash_Output"));
        EnsureDirectory(Combine(root, L"Restored_Extractor_Output"));
        if (mode == 3)
        {
            EnsureDirectory(Combine(root, L"StringHashDumper_Output"));
        }

        PrintLine(L"OK: prepared user workspace");
        PrintLine(L"MODE: user" + std::to_wstring(mode));
        PrintLine(L"WORKSPACE: " + root);

        if (mode == 1 && !options.extension.empty())
        {
            StaticHashGeneratorLite generator;
            StaticHashGeneratorLite::Result result{};
            std::wstring errorMessage;
            std::wstring outputDirectory = Combine(root, L"StaticHash_Output");
            if (!generator.GenerateFromExtension(options.extension, outputDirectory, result, errorMessage))
            {
                PrintLine(L"ERROR: " + errorMessage);
                return 3;
            }

            PrintLine(L"STATIC_HASH: generated");
            PrintLine(L"STATIC_HASH_OUTPUT: " + outputDirectory);
            PrintLine(L"RESOURCE_PATHS: " + std::to_wstring(result.resourcePathCount));
            PrintLine(L"DIRECTORY_HASHES: " + std::to_wstring(result.directoryHashCount));
            PrintLine(L"FILE_NAME_HASHES: " + std::to_wstring(result.fileNameHashCount));
        }
        else if (mode == 2 && !options.brand.empty())
        {
            std::wstring extensionsRoot = options.extensionsRoot;
            if (extensionsRoot.empty())
            {
                // CLI 通常位于 PythonWorkbench\core，因此默认 Extensions 在上一级目录。
                extensionsRoot = Combine(DirectoryName(AppDirectory()), L"Extensions");
            }

            StaticHashGeneratorLite generator;
            StaticHashGeneratorLite::Result result{};
            std::wstring errorMessage;
            std::wstring outputDirectory = Combine(root, L"StaticHash_Output");
            if (!generator.GenerateFromBrand(extensionsRoot, options.brand, outputDirectory, result, errorMessage))
            {
                PrintLine(L"ERROR: " + errorMessage);
                return 3;
            }

            PrintLine(L"STATIC_HASH: generated");
            PrintLine(L"STATIC_HASH_OUTPUT: " + outputDirectory);
            PrintLine(L"BRAND: " + options.brand);
            PrintLine(L"RESOURCE_PATHS: " + std::to_wstring(result.resourcePathCount));
            PrintLine(L"DIRECTORY_HASHES: " + std::to_wstring(result.directoryHashCount));
            PrintLine(L"FILE_NAME_HASHES: " + std::to_wstring(result.fileNameHashCount));
        }

        if (options.restore)
        {
            ResourceRestorerLite restorer;
            ResourceRestorerLite::Result restoreResult{};
            std::wstring errorMessage;
            if (!restorer.RestoreWorkspace(root, restoreResult, errorMessage))
            {
                PrintLine(L"RESTORE: failed");
                PrintLine(L"RESTORE_ERROR: " + errorMessage);
                return 4;
            }

            PrintLine(L"RESTORE: completed");
            PrintLine(L"RESTORE_REPORT: " + restoreResult.reportPath);
            PrintLine(L"TOTAL_FILES: " + std::to_wstring(restoreResult.totalFiles));
            PrintLine(L"RESTORED_FILES: " + std::to_wstring(restoreResult.restoredFiles));
            PrintLine(L"MISSING_DIRECTORY_HASH: " + std::to_wstring(restoreResult.missingDirectoryHash));
            PrintLine(L"MISSING_FILE_NAME_HASH: " + std::to_wstring(restoreResult.missingFileNameHash));
            PrintLine(L"COPY_FAILED: " + std::to_wstring(restoreResult.copyFailed));
        }
        else
        {
            PrintLine(L"NEXT_STEP: run dynamic-extract, then restore when Extractor_Output exists");
        }

        // 后续在这里接入旧版功能：
        // user1: 单游戏扩展集 -> 生成静态 Hash -> 动态 XP3 提取 -> 还原
        // user2: 会社集合 -> 合并候选 -> 生成静态 Hash -> 动态 XP3 提取 -> 还原
        // user3: 动态 XP3 提取 -> 动态 Hash 收集 -> 还原 -> 二次静态 Hash
        return 0;
    }

    int PreparePublisher(const Options& options)
    {
        if (options.game.empty())
        {
            PrintLine(L"ERROR: missing --game");
            return 2;
        }

        std::wstring gameDirectory = DirectoryName(options.game);
        std::wstring root = Combine(Combine(gameDirectory, L"Publisher"), L"ExtensionDraft");
        EnsureDirectory(root);

        PublisherExtensionBuilderLite builder;
        PublisherExtensionBuilderLite::Result result{};
        std::wstring errorMessage;
        if (!builder.BuildFromGameDirectory(options.game, root, options.brand, result, errorMessage))
        {
            PrintLine(L"PUBLISHER_DRAFT: failed");
            PrintLine(L"PUBLISHER_ERROR: " + errorMessage);
            return 6;
        }

        PrintLine(L"OK: prepared publisher workspace");
        PrintLine(L"MODE: publisher-make");
        PrintLine(L"WORKSPACE: " + root);
        PrintLine(L"PUBLISHER_DRAFT: generated");
        PrintLine(L"SOURCE_RESTORED: " + result.sourceRestoredDirectory);
        PrintLine(L"DRAFT_DIR: " + result.draftDirectory);
        PrintLine(L"DRAFT_REPORT: " + result.reportPath);
        PrintLine(L"RESOURCE_PATHS: " + std::to_wstring(result.resourcePathCount));
        PrintLine(L"DIRECTORY_NAMES: " + std::to_wstring(result.directoryNameCount));
        PrintLine(L"FILE_NAMES: " + std::to_wstring(result.fileNameCount));
        return 0;
    }

    int LaunchDynamic(const Options& options, const std::wstring& moduleFileName, const std::map<std::wstring, std::wstring>& extraEnvironment, bool waitForExit)
    {
        if (options.game.empty())
        {
            PrintLine(L"ERROR: missing --game");
            return 2;
        }

        DynamicLauncherLite launcher;
        DynamicLauncherLite::Result result{};
        std::wstring errorMessage;
        std::wstring moduleDirectory = AppDirectory();
        if (!launcher.Launch(moduleDirectory, options.game, moduleFileName, options.outputRoot, extraEnvironment, waitForExit, result, errorMessage))
        {
            PrintLine(L"DYNAMIC: failed");
            PrintLine(L"DYNAMIC_ERROR: " + errorMessage);
            return 5;
        }

        PrintLine(L"DYNAMIC: started");
        PrintLine(L"GAME: " + options.game);
        PrintLine(L"MODULE: " + result.modulePath);
        if (!options.outputRoot.empty())
        {
            PrintLine(L"OUTPUT_ROOT: " + options.outputRoot);
        }
        PrintLine(L"PROCESS_ID: " + std::to_wstring(result.processId));
        return 0;
    }

    int LaunchBatchExtract(const Options& options)
    {
        if (options.game.empty())
        {
            PrintLine(L"ERROR: missing --game");
            return 2;
        }
        if (options.outputRoot.empty())
        {
            PrintLine(L"ERROR: missing --output-root");
            return 2;
        }
        if (options.packageListFile.empty())
        {
            PrintLine(L"ERROR: missing --package-list");
            return 2;
        }

        DWORD attributes = GetFileAttributesW(options.packageListFile.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            PrintLine(L"ERROR: package list file not found");
            return 2;
        }

        std::map<std::wstring, std::wstring> environment;
        environment[L"CXDEC_BATCH_TASK_FILE"] = options.packageListFile;
        environment[L"CXDEC_BATCH_NOTIFY_POPUP"] = options.notifyPopup ? L"1" : L"0";
        environment[L"CXDEC_BATCH_NOTIFY_SOUND"] = options.notifySound ? L"1" : L"0";
        environment[L"CXDEC_BATCH_WORKERS"] = std::to_wstring(options.workerCount > 0 ? options.workerCount : 1);

        int code = LaunchDynamic(options, L"CxdecExtractorUI.dll", environment, false);
        if (code != 0)
        {
            return code;
        }

        PrintLine(L"BATCH_EXTRACT: started");
        PrintLine(L"TASK_FILE: " + options.packageListFile);
        PrintLine(L"WORKERS_REQUESTED: " + std::to_wstring(options.workerCount > 0 ? options.workerCount : 1));
        return 0;
    }

    int LaunchRestoreUiMode(const Options& options)
    {
        if (options.workspace.empty())
        {
            PrintLine(L"ERROR: missing --workspace");
            return 2;
        }

        std::wstring iconPath = Combine(DirectoryName(AppDirectory()), L"app_icon.ico");
        return RunRestoreUi(options.workspace,
                            options.workerCount > 0 ? (unsigned int)options.workerCount : 1u,
                            iconPath);
    }

    int LaunchLegacyStaticHash(const Options& options)
    {
        if (options.outputRoot.empty())
        {
            PrintLine(L"ERROR: missing --output-root");
            return 2;
        }

        std::wstring loaderPath = Combine(DirectoryName(AppDirectory()), L"CxdecExtractorLoader.exe");
        if (GetFileAttributesW(loaderPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            loaderPath = Combine(AppDirectory(), L"CxdecExtractorLoader.exe");
        }
        if (GetFileAttributesW(loaderPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            PrintLine(L"ERROR: CxdecExtractorLoader.exe not found");
            return 2;
        }

        EnsureDirectory(options.outputRoot);
        std::wstring workspaceMarker = Combine(options.outputRoot, L"workspace.exe");
        std::wstring commandLine = L"\"" + loaderPath + L"\" /statichash \"" + workspaceMarker + L"\"";

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION processInfo{};
        std::wstring mutableCommandLine = commandLine;
        std::wstring workingDirectory = DirectoryName(loaderPath);
        if (!CreateProcessW(loaderPath.c_str(),
                            mutableCommandLine.data(),
                            nullptr,
                            nullptr,
                            FALSE,
                            0,
                            nullptr,
                            workingDirectory.c_str(),
                            &startup,
                            &processInfo))
        {
            PrintLine(L"ERROR: failed to launch CxdecExtractorLoader.exe");
            return 5;
        }

        WaitForSingleObject(processInfo.hProcess, INFINITE);

        DWORD exitCode = 0;
        GetExitCodeProcess(processInfo.hProcess, &exitCode);
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);

        std::wstring outputDirectory = Combine(options.outputRoot, L"StaticHash_Output");
        std::wstring reportPath = Combine(outputDirectory, L"StaticHashReport.txt");
        if (exitCode != 0)
        {
            PrintLine(L"STATIC_HASH: failed");
            PrintLine(L"STATIC_HASH_OUTPUT: " + outputDirectory);
            return (int)exitCode;
        }

        PrintLine(L"STATIC_HASH: generated");
        PrintLine(L"STATIC_HASH_OUTPUT: " + outputDirectory);
        PrintLine(L"STATIC_HASH_REPORT: " + reportPath);
        return 0;
    }

    int LaunchPublisherTestUiMode(const Options& options)
    {
        if (options.outputRoot.empty())
        {
            PrintLine(L"ERROR: missing --output-root");
            return 2;
        }
        if (options.extension.empty())
        {
            PrintLine(L"ERROR: missing --extension");
            return 2;
        }
        std::wstring iconPath = Combine(DirectoryName(AppDirectory()), L"app_icon.ico");
        return RunPublisherTestUi(options.outputRoot, options.extension, iconPath);
    }

    bool HasVersionArg(int argc, wchar_t** argv)
    {
        for (int i = 1; i < argc; ++i)
        {
            std::wstring arg = argv[i];
            if (arg == L"--version" || arg == L"-v")
            {
                return true;
            }
        }
        return false;
    }
}

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);

    if (HasVersionArg(argc, argv))
    {
        PrintLine(std::wstring(L"CxdecCoreCLI ") + AppVersion);
        return 0;
    }

    Options options;
    if (!ParseArgs(argc, argv, options) || options.mode == L"help")
    {
        PrintHelp();
        return options.mode == L"help" ? 0 : 1;
    }

    if (options.mode == L"user1")
    {
        return PrepareUser(options, 1);
    }

    if (options.mode == L"user2")
    {
        return PrepareUser(options, 2);
    }

    if (options.mode == L"user3")
    {
        return PrepareUser(options, 3);
    }

    if (options.mode == L"publisher-make")
    {
        return PreparePublisher(options);
    }

    if (options.mode == L"dynamic-extract")
    {
        return LaunchDynamic(options, L"CxdecExtractorUI.dll", {}, false);
    }

    if (options.mode == L"batch-extract-xp3")
    {
        return LaunchBatchExtract(options);
    }

    if (options.mode == L"dynamic-stringhash")
    {
        return LaunchDynamic(options, L"CxdecStringDumper.dll", {}, false);
    }

    if (options.mode == L"dynamic-keydump")
    {
        return LaunchDynamic(options, L"CxdecKeyDumper.dll", {}, true);
    }

    if (options.mode == L"restore-ui")
    {
        return LaunchRestoreUiMode(options);
    }

    if (options.mode == L"publisher-test-ui")
    {
        return LaunchPublisherTestUiMode(options);
    }

    if (options.mode == L"static-hash")
    {
        return LaunchLegacyStaticHash(options);
    }

    PrintLine(L"ERROR: unknown mode " + options.mode);
    return 2;
}
