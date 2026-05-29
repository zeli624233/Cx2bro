#include "HashCore.h"
#include "pe.h"
#include "file.h"
#include "directory.h"
#include "path.h"
#include "stringhelper.h"
#include "ExtendUtils.h"
#include <map>

namespace Engine
{
    //**********IStringHasher***********//
    tjs_int IStringHasher::GetSaltLength() const
    {
        return this->mSaltSize;
    }

    const tjs_uint8* IStringHasher::GetSaltBytes() const
    {
        return this->mSalt;
    }

    IStringHasher::VptrTable* IStringHasher::GetVptrTable()
    {
        return *(IStringHasher::VptrTable**)this;
    }

    void IStringHasher::SetVptrTable(const VptrTable* vt)
    {
        *(const IStringHasher::VptrTable**)this = vt;
    }
    //================================//


    //*****************Dumper******************//

    static HashCore* g_Instance = nullptr;      //单实例
    HashCore::tCreateCompoundStorageMedia g_CreateStorageMediaFunc = nullptr;     //TVPStorageMedia[Cxdec]接口

    const CompoundStorageMedia* g_StorageMedia = nullptr;                        //封包管理媒体
    std::map<const void*, const IStringHasher::VptrTable*> g_PathNameHasherOriginalMap;   //每个hasher实例独立的原虚表指针
    std::map<const void*, const IStringHasher::VptrTable*> g_FileNameHasherOriginalMap;   //同上

    IStringHasher::VptrTable g_PathNameHasherHookVt;         //文件夹路径Hash Hook虚表
    IStringHasher::VptrTable g_FileNameHasherHookVt;         //文件名Hash Hook虚表

    tjs_int __fastcall HookPathNameHasherCalcute(IStringHasher* thisObj, void* unusedEdx, tTJSVariant* hashValueRet, const tTJSString* str, const tTJSString* seed);
    tjs_int __fastcall HookFileNameHasherCalcute(IStringHasher* thisObj, void* unusedEdx, tTJSVariant* hashValueRet, const tTJSString* str, const tTJSString* seed);

    //创建Media Hook
    tjs_error __cdecl HookCreateCompoundStorageMedia(CompoundStorageMedia** retTVPStorageMedia, tTJSVariant* tjsVarPrefix, int argc, void* argv)
    {
        tjs_error result = g_CreateStorageMediaFunc(retTVPStorageMedia, tjsVarPrefix, argc, argv);
        if (TJS_SUCCEEDED(result))
        {
            //获取媒体对象
            CompoundStorageMedia* storageMedia = *retTVPStorageMedia;
            g_StorageMedia = storageMedia;

            //打印Hash参数
            {
                HashCore* dumper = g_Instance;
                Log::Logger& uniLogger = dumper->mUniversalLogger;
                
                uniLogger.WriteUnicode(L"Hash Seed:%s\r\n", storageMedia->HasherSeed.c_str());
                uniLogger.WriteUnicode(L"PathNameHasherSalt:%s\r\n", StringHelper::BytesToHexStringW(storageMedia->PathNameHasher->GetSaltBytes(), storageMedia->PathNameHasher->GetSaltLength()).c_str());
                uniLogger.WriteUnicode(L"FileNameHasherSalt:%s\r\n", StringHelper::BytesToHexStringW(storageMedia->FileNameHasher->GetSaltBytes(), storageMedia->FileNameHasher->GetSaltLength()).c_str());
            }

            //文件夹路径Hash虚表Hook（每个hasher实例独立保存原始vtable）
            {
                IStringHasher* pnHasher = storageMedia->PathNameHasher;
                IStringHasher::VptrTable* pnHasherVt = pnHasher->GetVptrTable();

                // 首次遇到该实例才hook，已hook过的跳过
                if (g_PathNameHasherOriginalMap.find(pnHasher) == g_PathNameHasherOriginalMap.end())
                {
                    g_PathNameHasherOriginalMap[pnHasher] = pnHasherVt;
                    g_PathNameHasherHookVt = *pnHasherVt;
                    g_PathNameHasherHookVt.Calculate = HookPathNameHasherCalcute;
                    pnHasher->SetVptrTable(&g_PathNameHasherHookVt);
                }
            }

            //文件名Hash虚表Hook（每个hasher实例独立保存原始vtable）
            {
                IStringHasher* fnHasher = storageMedia->FileNameHasher;
                IStringHasher::VptrTable* fnHasherVt = fnHasher->GetVptrTable();

                if (g_FileNameHasherOriginalMap.find(fnHasher) == g_FileNameHasherOriginalMap.end())
                {
                    g_FileNameHasherOriginalMap[fnHasher] = fnHasherVt;
                    g_FileNameHasherHookVt = *fnHasherVt;
                    g_FileNameHasherHookVt.Calculate = HookFileNameHasherCalcute;
                    fnHasher->SetVptrTable(&g_FileNameHasherHookVt);
                }
            }
        }
        return result;
    }

    //文件夹路径Hash计算Hook fastcall模拟thiscall
    tjs_int __fastcall HookPathNameHasherCalcute(IStringHasher* thisObj, void* unusedEdx, tTJSVariant* hashValueRet, const tTJSString* str, const tTJSString* seed)
    {
        // 根据 this 指针查找当前 hasher 实例的原始虚表
        auto it = g_PathNameHasherOriginalMap.find(thisObj);
        const IStringHasher::VptrTable* originalVt = (it != g_PathNameHasherOriginalMap.end()) ? it->second : thisObj->GetVptrTable();
        tjs_int len = originalVt->Calculate(thisObj, nullptr, hashValueRet, str, seed);

        const wchar_t* relativeDirPath = str->c_str();
        //空文件夹替换
        if (*relativeDirPath == L'\0')
        {
            relativeDirPath = L"%EmptyString%";
        }

        tTJSVariantOctet* hashValue = hashValueRet->AsOctetNoAddRef();
        std::wstring hashHex = StringHelper::BytesToHexStringW(hashValue->GetData(), hashValue->GetLength());

        //打印 String[Sign]Hash[NewLine]
        g_Instance->mDirectoryHashLogger.WriteUnicode(L"%s%s%s\r\n", relativeDirPath, HashCore::Split, hashHex.c_str());

        // 发送 WM_COPYDATA 到收集器，实现实时日志显示
        g_Instance->SendHashToCollector(relativeDirPath, hashHex.c_str(), true);

        return len;
    }

    //文件名Hash计算Hook fastcall模拟thiscall
    tjs_int __fastcall HookFileNameHasherCalcute(IStringHasher* thisObj, void* unusedEdx, tTJSVariant* hashValueRet, const tTJSString* str, const tTJSString* seed)
    {
        // 根据 this 指针查找当前 hasher 实例的原始虚表
        auto it = g_FileNameHasherOriginalMap.find(thisObj);
        const IStringHasher::VptrTable* originalVt = (it != g_FileNameHasherOriginalMap.end()) ? it->second : thisObj->GetVptrTable();
        tjs_int len = originalVt->Calculate(thisObj, nullptr, hashValueRet, str, seed);

        const wchar_t* fileName = str->c_str();

        tTJSVariantOctet* hashValue = hashValueRet->AsOctetNoAddRef();
        std::wstring hashHex = StringHelper::BytesToHexStringW(hashValue->GetData(), hashValue->GetLength());

        //打印 String[Sign]Hash[NewLine]
        g_Instance->mFileNameHashLogger.WriteUnicode(L"%s%s%s\r\n", fileName, HashCore::Split, hashHex.c_str());

        // 发送 WM_COPYDATA 到收集器，实现实时日志显示
        g_Instance->SendHashToCollector(fileName, hashHex.c_str(), false);

        return len;
    }

    //================================//


    //**********HashCore***********//
    HashCore::HashCore()
    {
        mCollectorHwnd = nullptr;
        mHashLineMsg = 0;
    }

    HashCore::~HashCore()
    {
    }

    void HashCore::SetOutputDirectory(const std::wstring& directory)
    {
        std::wstring dumpOutDirectory = Path::Combine(directory, HashCore::FolderName);
        this->mDumperDirectoryPath = dumpOutDirectory;

        //创建输出目录
        Directory::Create(dumpOutDirectory);

        //日志初始化——不再删除旧日志，改为追加写入。
        //这样 Collector 可以跨游戏会话累积 hash 数据，不会"归零"。
        std::wstring directoryHashLogPath = Path::Combine(dumpOutDirectory, HashCore::DirectoryHashFileName);
        std::wstring fileNameHashLogPath = Path::Combine(dumpOutDirectory, HashCore::FileNameHashFileName);
        std::wstring universalLogPath = Path::Combine(dumpOutDirectory, HashCore::UniversalFileName);

        // 判断文件是否已存在（新文件才写 BOM 头）
        DWORD attrDir = GetFileAttributesW(directoryHashLogPath.c_str());
        bool dirLogExists = (attrDir != INVALID_FILE_ATTRIBUTES);
        DWORD attrFile = GetFileAttributesW(fileNameHashLogPath.c_str());
        bool fileLogExists = (attrFile != INVALID_FILE_ATTRIBUTES);
        DWORD attrUni = GetFileAttributesW(universalLogPath.c_str());
        bool uniLogExists = (attrUni != INVALID_FILE_ATTRIBUTES);

        this->mDirectoryHashLogger.Open(directoryHashLogPath.c_str());
        this->mFileNameHashLogger.Open(fileNameHashLogPath.c_str());
        this->mUniversalLogger.Open(universalLogPath.c_str());

        //只有新文件才写 UTF-16LE BOM 头
        if (!dirLogExists)
        {
            WORD bom = 0xFEFF;
            this->mDirectoryHashLogger.WriteData(&bom, sizeof(bom));
        }
        if (!fileLogExists)
        {
            WORD bom = 0xFEFF;
            this->mFileNameHashLogger.WriteData(&bom, sizeof(bom));
        }
        if (!uniLogExists)
        {
            WORD bom = 0xFEFF;
            this->mUniversalLogger.WriteData(&bom, sizeof(bom));
        }

        //读取收集器窗口句柄（由 CxdecDynamicHashCollector 在启动时设置）
        mCollectorHwnd = nullptr;
        wchar_t hwndStr[32] = {};
        if (::GetEnvironmentVariableW(L"CXDEC_COLLECTOR_HWND", hwndStr, 32) > 0)
        {
            mCollectorHwnd = (void*)(ULONG_PTR)_wtoi64(hwndStr);
        }

        //注册跨进程消息
        mHashLineMsg = (unsigned int)::RegisterWindowMessageW(L"CXDEC_HASH_LINE");
    }

    /// <summary>
    /// 发送 hash 行到收集器（跨进程 WM_COPYDATA，Windows 自动管理内存）
    /// </summary>
    void HashCore::SendHashToCollector(const wchar_t* text, const wchar_t* hashHex, bool isDir)
    {
        HWND hwnd = (HWND)mCollectorHwnd;
        if (!hwnd || !::IsWindow(hwnd))
            return;

        //格式: text##YSig##hashHex
        std::wstring full = std::wstring(text) + HashCore::Split + hashHex;

        COPYDATASTRUCT cds;
        cds.dwData = isDir ? 0 : 1;
        cds.cbData = (DWORD)((full.size() + 1) * sizeof(wchar_t));
        cds.lpData = (void*)full.c_str();

        //WM_COPYDATA 是同步的，但 Windows 自动管理内存跨进程传输
        ::SendMessageW(hwnd, WM_COPYDATA, (WPARAM)0, (LPARAM)&cds);
    }

    void HashCore::Initialize(PVOID codeVa, DWORD codeSize)
    {
        PVOID createMedia = PE::SearchPattern(codeVa, codeSize, HashCore::CreateCompoundStorageMediaSignature, sizeof(HashCore::CreateCompoundStorageMediaSignature) - 1);
        if (createMedia)
        {
            g_CreateStorageMediaFunc = (tCreateCompoundStorageMedia)createMedia;

            //Hook创建媒体接口
            HookUtils::InlineHook::Hook(g_CreateStorageMediaFunc, HookCreateCompoundStorageMedia);
        }
    }

    bool HashCore::IsInitialized()
    {
        return g_CreateStorageMediaFunc != nullptr;
    }

    //************=====Static=====************//

    HashCore* HashCore::GetInstance()
    {
        if (g_Instance == nullptr)
        {
            g_Instance = new HashCore();
        }
        return g_Instance;
    }

    void HashCore::Release()
    {
        if (g_Instance)
        {
            delete g_Instance;
            g_Instance = nullptr;
        }
    }
    //================================//
}
