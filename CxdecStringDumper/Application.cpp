#include "Application.h"
#include "path.h"
#include "util.h"
#include "ExtendUtils.h"

namespace Engine
{
    static Application* g_Instance = nullptr;

    // Global speedhack flag for GetProcAddress hook (controls audio mute)
    static bool g_SpeedHackEnabled = true;

    //Hook V2Link
    tTVPV2LinkProc g_V2Link = nullptr;
    HRESULT __stdcall HookV2Link(iTVPFunctionExporter* exporter)
    {
        HRESULT result = g_V2Link(exporter);
        HookUtils::InlineHook::UnHook(g_V2Link, HookV2Link);
        g_V2Link = nullptr;
        Application::GetInstance()->InitializeTVPEngine(exporter);
        return result;
    }

    //Hook LoadLibraryW (avoid conflict with hanization patches)
    typedef HMODULE (WINAPI *LoadLibraryWFunc)(LPCWSTR);
    static LoadLibraryWFunc g_OriginalLoadLibraryW = (LoadLibraryWFunc)::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HMODULE WINAPI HookLoadLibraryW(LPCWSTR lpLibFileName)
    {
        HMODULE result = g_OriginalLoadLibraryW(lpLibFileName);
        if (!result) return result;
        if (HIWORD(lpLibFileName) == 0) return result;
        if (wcsstr(lpLibFileName, L"CxdecStringDumper") != nullptr ||
            wcsstr(lpLibFileName, L"cXdecStringDumper") != nullptr)
            return result;
        return result;
    }

    //---- Speed hack: timeGetTime + WaitMessage + WFSO + audio mute ----
    // KrkrZ's timing uses timeGetTime, idle blocks on WaitMessage,
    // timer threads wait on WFSO. Audio causes skip to stall.
    // We hook all four + mute audio output.

    // Time
    typedef DWORD (WINAPI *TimeGetTimeFunc)();
    static TimeGetTimeFunc g_OriginalTimeGetTime = nullptr;

    // Idle block
    typedef BOOL (WINAPI *WaitMessageFunc)();
    static WaitMessageFunc g_OriginalWaitMessage = nullptr;

    // Thread waits
    typedef DWORD (WINAPI *WaitForSingleObjectFunc)(HANDLE, DWORD);
    static WaitForSingleObjectFunc g_OriginalWaitForSingleObject = nullptr;

    // Audio: DirectSoundCreate - prevent DirectSound from initializing
    // KrkrZ uses GetProcAddress to get DirectSoundCreate from dsound.dll
    // We intercept at GetProcAddress level instead.
    typedef HRESULT (WINAPI *DirectSoundCreateFunc)(const void*, void**, void*);
    static DirectSoundCreateFunc g_OriginalDirectSoundCreate = nullptr;
    static FARPROC g_RealDirectSoundCreateAddr = nullptr; // real function address

    typedef FARPROC (WINAPI *GetProcAddressFunc)(HMODULE, LPCSTR);
    static GetProcAddressFunc g_OriginalGetProcAddress = nullptr;

    static double g_SpeedMultiplier = 10.0;

    // Nuclear option: hook Sleep to 1ms max
    typedef void (WINAPI *SleepFunc)(DWORD);
    static SleepFunc g_OriginalSleep = (SleepFunc)::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "Sleep");

    // Hook SetTimer: make all timers fire at 1ms interval
    typedef UINT_PTR (WINAPI *SetTimerFunc)(HWND, UINT_PTR, UINT, TIMERPROC);
    static SetTimerFunc g_OriginalSetTimer = (SetTimerFunc)::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "SetTimer");

    // Hook PeekMessageW: inject synthetic mouse clicks to speed up skip
    typedef BOOL (WINAPI *PeekMessageWFunc)(LPMSG, HWND, UINT, UINT, UINT);
    static PeekMessageWFunc g_OriginalPeekMessageW = (PeekMessageWFunc)::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "PeekMessageW");
    static volatile LONG g_PeekCounter = 0;

    DWORD WINAPI HookTimeGetTime()
    {
        DWORD real = g_OriginalTimeGetTime();
        return (DWORD)((double)real * g_SpeedMultiplier);
    }

    BOOL WINAPI HookWaitMessage()
    {
        return FALSE; // Never block idle
    }

    DWORD WINAPI HookWaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds)
    {
        if (g_SpeedMultiplier > 1.0 && dwMilliseconds > 0 && dwMilliseconds != INFINITE)
        {
            DWORD newMs = (DWORD)((double)dwMilliseconds / g_SpeedMultiplier);
            if (newMs < 1) newMs = 1;
            return g_OriginalWaitForSingleObject(hHandle, newMs);
        }
        return g_OriginalWaitForSingleObject(hHandle, dwMilliseconds);
    }

    // Aggressive Sleep hook - max 1ms to kill all delays
    void WINAPI HookSleep(DWORD dwMilliseconds)
    {
        if (dwMilliseconds > 1) {
            g_OriginalSleep(1);
        } else {
            g_OriginalSleep(dwMilliseconds);
        }
    }

    // SetTimer hook: force 1ms interval
    UINT_PTR WINAPI HookSetTimer(HWND hWnd, UINT_PTR nIDEvent, UINT uElapse, TIMERPROC lpTimerFunc)
    {
        return g_OriginalSetTimer(hWnd, nIDEvent, 1, lpTimerFunc);
    }

    // PeekMessageW hook: every 50 empty polls, inject a click to speed skip
    BOOL WINAPI HookPeekMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg)
    {
        BOOL result = g_OriginalPeekMessageW(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg);
        if (!result && hWnd) {
            LONG cnt = InterlockedIncrement(&g_PeekCounter);
            if ((cnt % 50) == 0) {
                // Post a mouse click to simulate user interaction (speeds up skip)
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hWnd, &pt);
                WPARAM wp = MAKEWPARAM(pt.x, pt.y);
                LPARAM lp = MAKELPARAM(pt.x, pt.y);
                PostMessageW(hWnd, WM_LBUTTONDOWN, MK_LBUTTON, lp);
                PostMessageW(hWnd, WM_LBUTTONUP, 0, lp);
            }
        }
        return result;
    }

    // Prevent DirectSound init by intercepting GetProcAddress("DirectSoundCreate")
    HRESULT WINAPI HookDirectSoundCreate(const void* pcGuidDevice, void** ppDS, void* pUnkOuter)
    {
        return 0x80004005L; // E_FAIL
    }

    // Hook GetProcAddress: V2Link (init dumper) + DirectSoundCreate (mute audio)
    FARPROC WINAPI HookGetProcAddress(HMODULE hModule, LPCSTR lpProcName)
    {
        FARPROC result = g_OriginalGetProcAddress(hModule, lpProcName);

        if (result && HIWORD(lpProcName) != 0)
        {
            // Check V2Link - initialize dumper
            {
                const char* target = "V2Link";
                const char* p = lpProcName;
                bool match = true;
                for (int i = 0; i < 6; i++) { if (p[i] != target[i]) { match = false; break; } }
                if (match && p[6] == '\0') {
                    Application* app = Application::GetInstance();
                    if (app && !app->IsTVPEngineInitialize() && g_V2Link == nullptr) {
                        g_V2Link = (tTVPV2LinkProc)result;
                        HookUtils::InlineHook::Hook(g_V2Link, HookV2Link);
                        PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)hModule;
                        PIMAGE_NT_HEADERS nt = PIMAGE_NT_HEADERS((BYTE*)hModule + dos->e_lfanew);
                        DWORD optSize = nt->FileHeader.SizeOfOptionalHeader;
                        PIMAGE_SECTION_HEADER sec = (PIMAGE_SECTION_HEADER)((BYTE*)nt + sizeof(nt->Signature) + sizeof(IMAGE_FILE_HEADER) + optSize);
                        DWORD codeVa = (DWORD)(ULONG_PTR)hModule + sec->VirtualAddress;
                        DWORD codeSize = sec->SizeOfRawData;
                        HashCore* dumper = app->GetStringDumper();
                        if (dumper && !dumper->IsInitialized()) dumper->Initialize((PVOID)(ULONG_PTR)codeVa, codeSize);
                    }
                    return result;
                }
            }
            // Check DirectSoundCreate - mute audio (only when speedhack enabled)
            if (g_SpeedHackEnabled)
            {
                const char* target = "DirectSoundCreate";
                const char* p = lpProcName;
                bool match = true;
                for (int i = 0; i < 18; i++) { if (p[i] != target[i]) { match = false; break; } }
                if (match && p[18] == '\0') {
                    if (g_RealDirectSoundCreateAddr == nullptr) {
                        g_RealDirectSoundCreateAddr = result;
                        g_OriginalDirectSoundCreate = (DirectSoundCreateFunc)result;
                    }
                    return (FARPROC)HookDirectSoundCreate;
                }
            }
        }
        return result;
    }

    void InitSpeedHack()
    {
        HANDLE hLog = ::CreateFileW(L"SpeedHack_Debug.log", GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        auto LogMsg = [&](const wchar_t* msg) {
            ::OutputDebugStringW(msg);
            if (hLog != INVALID_HANDLE_VALUE) {
                DWORD w; ::WriteFile(hLog, msg, (DWORD)(wcslen(msg)*sizeof(wchar_t)), &w, nullptr);
            }
        };

        g_SpeedMultiplier = 10.0;
        LogMsg(L"[SpeedHack] Init (10x + audio mute)...\r\n");

        // Hook timeGetTime
        HMODULE winmm = ::GetModuleHandleW(L"winmm.dll");
        if (winmm) {
            g_OriginalTimeGetTime = (TimeGetTimeFunc)::GetProcAddress(winmm, "timeGetTime");
            if (g_OriginalTimeGetTime) {
                HookUtils::InlineHook::Hook(g_OriginalTimeGetTime, HookTimeGetTime);
                LogMsg(L"[SpeedHack] timeGetTime hooked\r\n");
            }
            // Hook Sleep (nuclear: max 1ms for all sleep calls)
            g_OriginalSleep = (SleepFunc)::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "Sleep");
            if (g_OriginalSleep) {
                HookUtils::InlineHook::Hook(g_OriginalSleep, HookSleep);
                LogMsg(L"[SpeedHack] Sleep hooked (max 1ms)\r\n");
            }
            // Hook SetTimer: all timers fire at 1ms
            g_OriginalSetTimer = (SetTimerFunc)::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "SetTimer");
            if (g_OriginalSetTimer) {
                HookUtils::InlineHook::Hook(g_OriginalSetTimer, HookSetTimer);
                LogMsg(L"[SpeedHack] SetTimer hooked (1ms)\r\n");
            }
            // Hook PeekMessageW: inject clicks every 50 empty polls
            g_OriginalPeekMessageW = (PeekMessageWFunc)::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "PeekMessageW");
            if (g_OriginalPeekMessageW) {
                HookUtils::InlineHook::Hook(g_OriginalPeekMessageW, HookPeekMessageW);
                LogMsg(L"[SpeedHack] PeekMessageW hooked (click inject)\r\n");
            }
        }

        // Hook WaitMessage
        g_OriginalWaitMessage = (WaitMessageFunc)::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "WaitMessage");
        if (g_OriginalWaitMessage) {
            HookUtils::InlineHook::Hook(g_OriginalWaitMessage, HookWaitMessage);
            LogMsg(L"[SpeedHack] WaitMessage hooked (no idle)\r\n");
        }

        // Hook WaitForSingleObject
        g_OriginalWaitForSingleObject = (WaitForSingleObjectFunc)::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "WaitForSingleObject");
        if (g_OriginalWaitForSingleObject) {
            HookUtils::InlineHook::Hook(g_OriginalWaitForSingleObject, HookWaitForSingleObject);
            LogMsg(L"[SpeedHack] WFSO hooked (waits shortened)\r\n");
        }

        LogMsg(L"[SpeedHack] Init done\r\n");
        if (hLog != INVALID_HANDLE_VALUE) ::CloseHandle(hLog);
    }

    //Application class
    Application::Application()
    {
        this->mCurrentDirectoryPath = Path::GetDirectoryName(Util::GetModulePathW(::GetModuleHandleW(NULL)));
        wchar_t outputRoot[MAX_PATH]{};
        if (::GetEnvironmentVariableW(L"CXDEC_OUTPUT_ROOT", outputRoot, MAX_PATH) > 0)
        {
            this->mCurrentDirectoryPath = outputRoot;
        }
        this->mTVPExporterInitialized = false;
        this->mStringDumper = HashCore::GetInstance();
        this->mStringDumper->SetOutputDirectory(this->mCurrentDirectoryPath);
    }

    Application::~Application()
    {
        if (this->mStringDumper)
        {
            HashCore::Release();
            this->mStringDumper = nullptr;
        }
    }

    void Application::InitializeModule(HMODULE hModule)
    {
        this->mModuleDirectoryPath = Path::GetDirectoryName(Util::GetModulePathW(hModule));
    }

    void Application::InitializeTVPEngine(iTVPFunctionExporter* exporter)
    {
        this->mTVPExporterInitialized = TVPInitImportStub(exporter);
        TVPSetCommandLine(L"-debugwin", L"yes");
        TVPSetCommandLine(L"-contfreq", L"0"); // Remove continuous event frequency limit
    }

    bool Application::IsTVPEngineInitialize()
    {
        return this->mTVPExporterInitialized;
    }

    HashCore* Application::GetStringDumper()
    {
        return this->mStringDumper;
    }

    Application* Application::GetInstance()
    {
        return g_Instance;
    }

    void Application::Initialize(HMODULE hModule)
    {
        g_Instance = new Application();
        g_Instance->InitializeModule(hModule);

        // 检查环境变量：CXDEC_SPEED_HACK=1 时启动加速（默认开启）
        wchar_t speedHackBuf[8] = {};
        g_SpeedHackEnabled = false;
        if (GetEnvironmentVariableW(L"CXDEC_SPEED_HACK", speedHackBuf, 8) > 0)
        {
            g_SpeedHackEnabled = (wcscmp(speedHackBuf, L"1") == 0);
        }
        if (g_SpeedHackEnabled)
        {
            InitSpeedHack();
        }

        // 始终安装 GetProcAddress Hook，拦截 V2Link 以初始化 Dumper
        // 注意：这是 HashCore::Initialize() 被调用的唯一入口
        // 之前这个 hook 放在 InitSpeedHack() 内部，导致加速关闭时
        // hasher 的虚表 hook 永远不被安装，实时日志不工作
        g_OriginalGetProcAddress = (GetProcAddressFunc)::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "GetProcAddress");
        if (g_OriginalGetProcAddress)
        {
            HookUtils::InlineHook::Hook(g_OriginalGetProcAddress, HookGetProcAddress);
        }

        HookUtils::InlineHook::Hook(g_OriginalLoadLibraryW, HookLoadLibraryW);
    }

    void Application::Release()
    {
        if (g_Instance)
        {
            delete g_Instance;
            g_Instance = nullptr;
        }
    }
}