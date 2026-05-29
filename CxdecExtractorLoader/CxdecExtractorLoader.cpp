#include <windows.h>
#include <commctrl.h>
#include <detours.h>
#include <string>

#include "loaderipc.h"
#include "path.h"
#include "util.h"
#include "directory.h"
#include "encoding.h"
#include "resource.h"
#include "ExtractorOutputRestorer.h"
#include "StaticHashGenerator.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/MERGE:\".detourd=.data\"")
#pragma comment(linker, "/MERGE:\".detourc=.rdata\"")

#ifdef _UNICODE
#if defined _M_IX86
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='x86' publicKeyToken='6595b64144ccf1df' language='*'\"")
#elif defined _M_X64
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'\"")
#else
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif
#endif

static std::wstring g_LoaderFullPath;
static std::wstring g_LoaderCurrentDirectory;
static std::wstring g_KrkrExeFullPath;
static std::wstring g_KrkrExeDirectory;

namespace
{
    void SetLoaderWindowHandleEnv(HWND hwnd)
    {
        std::wstring value = std::to_wstring((unsigned long long)(ULONG_PTR)hwnd);
        ::SetEnvironmentVariableW(LoaderIpc::LoaderWindowHandleEnvName, value.c_str());
    }

    void ClearLoaderWindowHandleEnv()
    {
        ::SetEnvironmentVariableW(LoaderIpc::LoaderWindowHandleEnvName, nullptr);
    }

    void SetProgressPercentText(HWND hwnd, unsigned int percent)
    {
        wchar_t text[16]{};
        wsprintfW(text, L"%u%%", percent);
        ::SetWindowTextW(::GetDlgItem(hwnd, IDC_KeyProgressText), text);
    }

    void ShowKeyProgressControls(HWND hwnd, bool visible)
    {
        int showMode = visible ? SW_SHOW : SW_HIDE;
        ::ShowWindow(::GetDlgItem(hwnd, IDC_KeyProgress), showMode);
        ::ShowWindow(::GetDlgItem(hwnd, IDC_KeyProgressText), showMode);
        ::ShowWindow(::GetDlgItem(hwnd, IDC_KeyProgressLabel), showMode);
    }

    void InitializeKeyProgressControls(HWND hwnd)
    {
        HWND progressBar = ::GetDlgItem(hwnd, IDC_KeyProgress);
        if (progressBar)
        {
            ::SendMessageW(progressBar, PBM_SETRANGE, 0u, MAKELPARAM(0, 100));
            ::SendMessageW(progressBar, PBM_SETPOS, 0u, 0u);
        }

        ::SetWindowTextW(::GetDlgItem(hwnd, IDC_KeyProgressLabel), L"\x63D0\x53D6\x8FDB\x5EA6");
        SetProgressPercentText(hwnd, 0u);
    }

    void UpdateKeyProgressUi(HWND hwnd, unsigned int percent, const wchar_t* labelText)
    {
        if (percent > 100u)
        {
            percent = 100u;
        }

        HWND progressBar = ::GetDlgItem(hwnd, IDC_KeyProgress);
        if (progressBar)
        {
            ::SendMessageW(progressBar, PBM_SETPOS, (WPARAM)percent, 0u);
        }

        if (labelText)
        {
            ::SetWindowTextW(::GetDlgItem(hwnd, IDC_KeyProgressLabel), labelText);
        }

        SetProgressPercentText(hwnd, percent);
    }

    std::wstring FormatRestorePercent(unsigned int value, unsigned int total)
    {
        if (total == 0)
        {
            return L"0.00%";
        }

        unsigned int scaled = (unsigned int)(((unsigned long long)value * 10000ull + total / 2u) / total);
        wchar_t text[32]{};
        wsprintfW(text, L"%u.%02u%%", scaled / 100u, scaled % 100u);
        return text;
    }

    void RestoreExtractorOutput(HWND hwnd)
    {
        if (g_KrkrExeDirectory.empty())
        {
            ::MessageBoxW(hwnd,
                          L"游戏目录无效，无法还原资源文件名。",
                          L"错误",
                          MB_OK | MB_ICONERROR);
            return;
        }

        ::EnableWindow(::GetDlgItem(hwnd, IDC_RestoreExtractorOutput), FALSE);

        ExtractorOutputRestorer restorer(g_KrkrExeDirectory);
        ExtractorOutputRestorer::Result result{};
        std::wstring errorMessage;
        bool success = restorer.Restore(result, errorMessage);

        ::EnableWindow(::GetDlgItem(hwnd, IDC_RestoreExtractorOutput), TRUE);

        if (!success)
        {
            ::MessageBoxW(hwnd,
                          errorMessage.empty() ? L"还原资源文件名失败。" : errorMessage.c_str(),
                          L"错误",
                          MB_OK | MB_ICONERROR);
            return;
        }

        std::wstring message;
        message += L"还原完成。\r\n\r\n";
        message += L"输出目录：Restored_Extractor_Output\r\n";
        message += L"详细报告：RestoreReport.txt\r\n\r\n";
        message += L"总文件数：" + std::to_wstring(result.TotalFiles) + L"\r\n";
        message += L"成功还原：" + std::to_wstring(result.RestoredFiles) + L"\r\n";
        message += L"成功率：" + FormatRestorePercent(result.RestoredFiles, result.TotalFiles) + L"\r\n";
        message += L"缺少目录Hash：" + std::to_wstring(result.MissingDirectoryHash) + L"\r\n";
        message += L"缺少文件名Hash：" + std::to_wstring(result.MissingFileNameHash) + L"\r\n";
        message += L"复制失败：" + std::to_wstring(result.CopyFailed) + L"\r\n";

        if (!result.Directories.empty())
        {
            message += L"\r\n各目录还原情况：\r\n";
            for (const auto& directory : result.Directories)
            {
                message += L"\r\n";
                message += directory.Name + L"\r\n";
                message += L"  总文件数：" + std::to_wstring(directory.TotalFiles);
                message += L"，成功：" + std::to_wstring(directory.RestoredFiles);
                message += L"，成功率：" + FormatRestorePercent(directory.RestoredFiles, directory.TotalFiles) + L"\r\n";
                message += L"  缺少目录Hash：" + std::to_wstring(directory.MissingDirectoryHash);
                message += L"，缺少文件名Hash：" + std::to_wstring(directory.MissingFileNameHash);
                message += L"，复制失败：" + std::to_wstring(directory.CopyFailed) + L"\r\n";
            }
        }

        ::MessageBoxW(hwnd, message.c_str(), L"CxdecExtractorLoader", MB_OK | MB_ICONINFORMATION);
    }

    void GenerateStaticHashMapping(HWND hwnd)
    {
        if (g_KrkrExeDirectory.empty())
        {
            ::MessageBoxW(hwnd,
                          L"游戏目录无效，无法生成静态Hash映射。",
                          L"错误",
                          MB_OK | MB_ICONERROR);
            return;
        }

        ::EnableWindow(::GetDlgItem(hwnd, IDC_StaticHashGenerator), FALSE);

        StaticHashGenerator generator(g_KrkrExeDirectory);
        StaticHashGenerator::Result result{};
        std::wstring errorMessage;
        bool success = generator.Generate(result, errorMessage);

        ::EnableWindow(::GetDlgItem(hwnd, IDC_StaticHashGenerator), TRUE);

        if (!success)
        {
            ::MessageBoxW(hwnd,
                          errorMessage.empty() ? L"静态生成Hash映射失败。" : errorMessage.c_str(),
                          L"错误",
                          MB_OK | MB_ICONERROR);
            return;
        }

        std::wstring message;
        message += L"静态Hash映射生成完成。\r\n\r\n";
        message += L"输出目录：StaticHash_Output\r\n";
        message += L"详细报告：StaticHashReport.txt\r\n\r\n";
        message += L"ResourcePaths.txt 行数：" + std::to_wstring(result.ResourcePathCount) + L"\r\n";
        message += L"DirectoryNames.txt 行数：" + std::to_wstring(result.DirectoryNameCount) + L"\r\n";
        message += L"FileNames.txt 行数：" + std::to_wstring(result.FileNameCount) + L"\r\n";
        message += L"已还原路径扫描数：" + std::to_wstring(result.RestoredPathCount) + L"\r\n\r\n";
        message += L"生成目录Hash数：" + std::to_wstring(result.DirectoryHashCount) + L"\r\n";
        message += L"生成文件名Hash数：" + std::to_wstring(result.FileNameHashCount) + L"\r\n";

        ::MessageBoxW(hwnd, message.c_str(), L"CxdecExtractorLoader", MB_OK | MB_ICONINFORMATION);
    }

    bool RunRestoreCommandLine(const std::wstring& gameDirectory)
    {
        ExtractorOutputRestorer restorer(gameDirectory);
        ExtractorOutputRestorer::Result result{};
        std::wstring errorMessage;
        bool success = restorer.Restore(result, errorMessage);
        if (!success)
        {
            ::MessageBoxW(nullptr,
                          errorMessage.empty() ? L"还原资源文件名失败。" : errorMessage.c_str(),
                          L"错误",
                          MB_OK | MB_ICONERROR);
            return false;
        }

        return true;
    }

    bool RunStaticHashCommandLine(const std::wstring& gameDirectory)
    {
        StaticHashGenerator generator(gameDirectory);
        StaticHashGenerator::Result result{};
        std::wstring errorMessage;
        bool success = generator.Generate(result, errorMessage);
        if (!success)
        {
            ::MessageBoxW(nullptr,
                          errorMessage.empty() ? L"静态生成Hash映射失败。" : errorMessage.c_str(),
                          L"错误",
                          MB_OK | MB_ICONERROR);
            return false;
        }

        return true;
    }

    bool RunInjectCommandLine(const std::wstring& loaderDirectory,
                              const std::wstring& gameExePath,
                              const std::wstring& gameDirectory,
                              const std::wstring& injectDllFileName,
                              bool waitForProcess)
    {
        std::wstring injectDllFullPath = Path::Combine(loaderDirectory, injectDllFileName);
        if (!Path::Exists(injectDllFullPath))
        {
            ::MessageBoxW(nullptr, (L"找不到模块：" + injectDllFullPath).c_str(), L"错误", MB_OK | MB_ICONERROR);
            return false;
        }

        std::string injectDllFullPathA = Encoding::UnicodeToAnsi(injectDllFullPath, Encoding::CodePage::ACP);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};

        if (!DetourCreateProcessWithDllW(gameExePath.c_str(),
                                         NULL,
                                         NULL,
                                         NULL,
                                         FALSE,
                                         0u,
                                         NULL,
                                         gameDirectory.c_str(),
                                         &si,
                                         &pi,
                                         injectDllFullPathA.c_str(),
                                         NULL))
        {
            ::MessageBoxW(nullptr, L"创建进程失败。", L"错误", MB_OK | MB_ICONERROR);
            return false;
        }

        if (waitForProcess)
        {
            ::WaitForSingleObject(pi.hProcess, INFINITE);
        }

        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        return true;
    }
}

INT_PTR CALLBACK LoaderDialogWindProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == LoaderIpc::ProgressMessage())
    {
        ShowKeyProgressControls(hwnd, true);
        UpdateKeyProgressUi(hwnd, (unsigned int)wParam, L"\x63D0\x53D6\x8FDB\x5EA6");
        return TRUE;
    }

    if (msg == LoaderIpc::CompletedMessage())
    {
        ShowKeyProgressControls(hwnd, true);
        UpdateKeyProgressUi(hwnd, 100u, L"\x63D0\x53D6\x5B8C\x6210");
        ::MessageBoxW(hwnd,
                      L"\x63D0\x53D6\x5B8C\x6210\xFF0C\x8BF7\x67E5\x770B\x76EE\x5F55\x3002",
                      L"CxdecExtractorLoader",
                      MB_OK | MB_ICONINFORMATION);
        ::PostMessageW(hwnd, WM_CLOSE, 0u, 0u);
        return TRUE;
    }

    switch (msg)
    {
        case WM_INITDIALOG:
        {
            InitializeKeyProgressControls(hwnd);
            ShowKeyProgressControls(hwnd, false);
            return TRUE;
        }
        case WM_COMMAND:
        {
            std::wstring injectDllFileName;
            bool shouldCloseLoaderAfterLaunch = true;

            switch (LOWORD(wParam))
            {
                case IDC_Extractor:
                    injectDllFileName = L"CxdecExtractorUI.dll";
                    break;
                case IDC_StringDumper:
                    injectDllFileName = L"CxdecStringDumper.dll";
                    break;
                case IDC_KeyDumper:
                    injectDllFileName = L"CxdecKeyDumper.dll";
                    shouldCloseLoaderAfterLaunch = false;
                    break;
                case IDC_RestoreExtractorOutput:
                    RestoreExtractorOutput(hwnd);
                    break;
                case IDC_StaticHashGenerator:
                    GenerateStaticHashMapping(hwnd);
                    break;
            }

            if (!injectDllFileName.empty())
            {
                std::wstring injectDllFullPath = Path::Combine(g_LoaderCurrentDirectory, injectDllFileName);
                std::string injectDllFullPathA = Encoding::UnicodeToAnsi(injectDllFullPath, Encoding::CodePage::ACP);

                STARTUPINFOW si{};
                si.cb = sizeof(si);
                PROCESS_INFORMATION pi{};

                if (!shouldCloseLoaderAfterLaunch)
                {
                    SetLoaderWindowHandleEnv(hwnd);
                }

                if (DetourCreateProcessWithDllW(g_KrkrExeFullPath.c_str(),
                                                NULL,
                                                NULL,
                                                NULL,
                                                FALSE,
                                                0u,
                                                NULL,
                                                g_KrkrExeDirectory.c_str(),
                                                &si,
                                                &pi,
                                                injectDllFullPathA.c_str(),
                                                NULL))
                {
                    ::CloseHandle(pi.hThread);
                    ::CloseHandle(pi.hProcess);

                    if (shouldCloseLoaderAfterLaunch)
                    {
                        ::PostMessageW(hwnd, WM_CLOSE, 0u, 0u);
                    }
                    else
                    {
                        ::EnableWindow(::GetDlgItem(hwnd, IDC_Extractor), FALSE);
                        ::EnableWindow(::GetDlgItem(hwnd, IDC_StringDumper), FALSE);
                        ::EnableWindow(::GetDlgItem(hwnd, IDC_KeyDumper), FALSE);
                        ::EnableWindow(::GetDlgItem(hwnd, IDC_RestoreExtractorOutput), FALSE);
                        ::EnableWindow(::GetDlgItem(hwnd, IDC_StaticHashGenerator), FALSE);
                        ShowKeyProgressControls(hwnd, true);
                        UpdateKeyProgressUi(hwnd, 0u, L"\x7B49\x5F85\x63D0\x53D6\x5F00\x59CB");
                        ::SetWindowTextW(hwnd, L"CxdecExtractorLoader - \x7B49\x5F85Key\x63D0\x53D6\x5B8C\x6210");
                    }
                }
                else
                {
                    if (!shouldCloseLoaderAfterLaunch)
                    {
                        ClearLoaderWindowHandleEnv();
                    }
                    ::MessageBoxW(hwnd,
                                  L"\x521B\x5EFA\x8FDB\x7A0B\x9519\x8BEF",
                                  L"\x9519\x8BEF",
                                  MB_OK | MB_ICONERROR);
                }
            }
            return TRUE;
        }
        case WM_CLOSE:
        {
            ::DestroyWindow(hwnd);
            return TRUE;
        }
        case WM_DESTROY:
        {
            ClearLoaderWindowHandleEnv();
            ::PostQuitMessage(0);
            return TRUE;
        }
    }

    return FALSE;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nShowCmd)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(nShowCmd);

    INITCOMMONCONTROLSEX commonControls{ sizeof(commonControls), ICC_PROGRESS_CLASS };
    ::InitCommonControlsEx(&commonControls);

    std::wstring loaderFullPath = Util::GetAppPathW();
    std::wstring loaderCurrentDirectory = Path::GetDirectoryName(loaderFullPath);
    std::wstring krkrExeFullPath;
    std::wstring krkrExeDirectory;

    {
        int argc = 0;
        LPWSTR* argv = ::CommandLineToArgvW(lpCmdLine, &argc);
        if (argc >= 2 && (lstrcmpiW(argv[0], L"/restore") == 0 ||
                          lstrcmpiW(argv[0], L"/statichash") == 0 ||
                          lstrcmpiW(argv[0], L"/extract") == 0 ||
                          lstrcmpiW(argv[0], L"/stringhash") == 0 ||
                          lstrcmpiW(argv[0], L"/keydump") == 0))
        {
            krkrExeFullPath = std::wstring(argv[1]);
            krkrExeDirectory = Path::GetDirectoryName(krkrExeFullPath);

            bool success = false;
            if (lstrcmpiW(argv[0], L"/restore") == 0)
            {
                success = RunRestoreCommandLine(krkrExeDirectory);
            }
            else
            {
                if (lstrcmpiW(argv[0], L"/statichash") == 0)
                {
                    success = RunStaticHashCommandLine(krkrExeDirectory);
                }
                else if (lstrcmpiW(argv[0], L"/extract") == 0)
                {
                    success = RunInjectCommandLine(loaderCurrentDirectory, krkrExeFullPath, krkrExeDirectory, L"CxdecExtractorUI.dll", false);
                }
                else if (lstrcmpiW(argv[0], L"/stringhash") == 0)
                {
                    success = RunInjectCommandLine(loaderCurrentDirectory, krkrExeFullPath, krkrExeDirectory, L"CxdecStringDumper.dll", false);
                }
                else if (lstrcmpiW(argv[0], L"/keydump") == 0)
                {
                    success = RunInjectCommandLine(loaderCurrentDirectory, krkrExeFullPath, krkrExeDirectory, L"CxdecKeyDumper.dll", true);
                }
            }

            ::LocalFree(argv);
            return success ? 0 : 1;
        }

        if (argc)
        {
            krkrExeFullPath = std::wstring(argv[0]);
            krkrExeDirectory = Path::GetDirectoryName(krkrExeFullPath);
        }
        ::LocalFree(argv);
    }

    g_LoaderFullPath = loaderFullPath;
    g_LoaderCurrentDirectory = loaderCurrentDirectory;
    g_KrkrExeFullPath = krkrExeFullPath;
    g_KrkrExeDirectory = krkrExeDirectory;

    if (!krkrExeFullPath.empty() && krkrExeFullPath != loaderFullPath)
    {
        HWND hwnd = ::CreateDialogParamW((HINSTANCE)hInstance, MAKEINTRESOURCEW(IDD_MainForm), NULL, LoaderDialogWindProc, 0u);
        ::ShowWindow(hwnd, SW_NORMAL);

        MSG msg{};
        while (BOOL ret = ::GetMessageW(&msg, NULL, 0u, 0u))
        {
            if (ret == -1)
            {
                return -1;
            }

            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }
    else
    {
        ::MessageBoxW(nullptr,
                      L"\x8BF7\x62D6\x62FD\x6E38\x620F\x4E3B\x7A0B\x5E8F\x5230\x542F\x52A8\x5668",
                      L"\x9519\x8BEF",
                      MB_OK | MB_ICONERROR);
    }

    return 0;
}
