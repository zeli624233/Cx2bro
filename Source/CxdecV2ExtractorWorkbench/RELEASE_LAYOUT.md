# 发布目录结构

正式发布时保留完整目录，不要只复制单独的 exe。

```text
CxdecV2ExtractorWorkbench\
  CxdecV2ExtractorWorkbench.exe
  README.md
  RELEASE_LAYOUT.md
  app_icon.ico
  _runtime\
  core\
    CxdecCoreCLI.exe
    CxdecExtractor.dll
    CxdecExtractorUI.dll
    CxdecStringDumper.dll
    CxdecKeyDumper.dll
  Extensions\
    会社\
      游戏\
        manifest.int
        rules.int
```

不进入正式发布包：

```text
__pycache__\
Cache\
build\
release\
*.bak*
startup_error_log.txt
测试游戏输出目录
```

`_runtime` 由 PyInstaller 生成，包含 Python 和 PySide6 运行时。`core` 保存 C++ 核心程序和动态模块。`Extensions` 保存内置扩展集库，新版扩展集目录只保留 `manifest.int` 与 `rules.int`。

构建正式发布包：

```powershell
.\build_release.ps1
```
