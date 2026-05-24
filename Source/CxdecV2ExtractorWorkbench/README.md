# CxdecV2ExtractorWorkbench

CxdecV2ExtractorWorkbench 是一个面向 Kirikiri / XP3 游戏资源整理与资源名还原的工作台。它把动态 XP3 提取、字符串 Hash 收集、静态 Hash 映射生成、资源文件名还原、扩展集制作与扩展集管理放在同一个流程里，让使用者可以从“只能得到 hash 文件名的解包结果”逐步走到“可读目录与可读文件名”的整理结果。

项目采用 Python + PySide6 作为图形界面和流程编排层，C++ 核心程序负责更接近底层的工作：动态模块加载、XP3 资源提取、字符串 Hash 收集、Key 提取、静态 Hash 生成、资源名还原，以及扩展集测试窗口。



## 代码来源与许可证说明

本仓库是由 **ユイ可愛ね / zeli624233** 维护的独立仓库，不是 GitHub 上对 `YeLikesss / KrkrExtractForCxdecV2` 的 Fork 仓库。

但本项目的 CxdecV2 / krkrz_hxv4 底层提取、动态模块、Hash 处理等核心实现，沿用了 / 改写了 **YeLikesss / YeLike** 的开源项目 **KrkrExtractForCxdecV2** 的部分 C++ 代码与实现思路：

- 原项目：`KrkrExtractForCxdecV2`
- 原作者：`YeLikesss / YeLike`
- 原仓库：<https://github.com/YeLikesss/KrkrExtractForCxdecV2>
- 原项目许可证：`AGPL-3.0`

因此，本项目建议继续以 **AGPL-3.0** 方式发布，并保留原作者署名、项目链接、许可证说明和源码提供义务。这里的许可证继承原因是“代码来源和派生关系”，不是 GitHub 仓库是否显示为 Fork。

当前工作台中由 zeli624233 补充和维护的部分，主要包括图形化工作台流程、扩展集制作/测试/安装逻辑、`manifest.int` / `rules.int` 发布流程、静态 Hash 复用流程和相关 UI 整理。

## 项目定位

这个工具的目标不是替代所有解包器，而是解决 Cxdec 工作流里最麻烦的一段：资源可以提取出来，但文件名和目录名往往只剩 Hash。工作台通过扩展集、动态 Hash 收集和规则推导，把已知命名规律沉淀成可复用规则，并在后续作品中反复利用。

它适合两类人：

- 使用者：想对某个游戏执行资源提取、Hash 收集和文件名还原。
- 扩展集制作者：想把一次成功的还原结果整理成可分享、可复用、体积尽量小的扩展集。

## 主要功能

### 功能 1：根据该作扩展集提取

适用于已经有当前游戏专用扩展集的情况。

流程：

1. 选择游戏 exe。
2. 选择对应会社和游戏扩展集。
3. 根据扩展集生成静态 Hash 映射到 `User\1\StaticHash_Output`。
4. 动态提取 XP3 资源到 `User\1\Extractor_Output`。
5. 使用静态 Hash 映射还原资源名，输出到 `User\1\Restored_Extractor_Output`。

### 功能 2：根据会社集合扩展集尝试新作

适用于新作还没有专用扩展集，但同会社旧作命名规律可复用的情况。

流程：

1. 选择会社集合扩展集。
2. 合并该会社下已有扩展集规则，生成静态 Hash 映射到 `User\2\StaticHash_Output`。
3. 动态提取 XP3 资源到 `User\2\Extractor_Output`。
4. 使用集合 Hash 映射尝试还原新作，输出到 `User\2\Restored_Extractor_Output`。

这个模式的依据是：同一会社的资源目录、背景、立绘、语音、系统图等命名方式常常有延续性。即使不能完整命中，也可能显著减少需要动态 Hash 收集的范围。

### 功能 3：传统动态模式

适用于功能 1 / 功能 2 都不够用、没有可靠扩展集，或还原率不足的情况。

流程：

1. 动态提取 XP3 资源到 `User\3\Extractor_Output`。
2. 加载字符串 Hash 收集模块，进入游戏触发资源访问，输出到 `User\3\StringHashDumper_Output`。
3. 根据 `Extractor_Output` 和 `StringHashDumper_Output` 执行资源名还原，输出到 `User\3\Restored_Extractor_Output`。

功能 3 也是制作扩展集的重要前置步骤。还原结果越完整，后续规则推导越容易，正式扩展集也越小。

## 扩展集系统

扩展集是这个工作台的核心复用单位。它记录某个游戏或某个会社的资源命名规律、HashSeed、目录/文件名候选和必要的验证信息。使用扩展集后，工作台可以不依赖每次都进游戏重新跑动态 Hash，而是直接生成静态 Hash 映射。

当前新版扩展集采用极简 int 格式：

```text
manifest.int
rules.int
```

其中：

- `manifest.int` 保存扩展集元信息，例如会社、游戏名、HashSeed、包模式、资源数量和验证摘要。
- `rules.int` 保存规则、目录补丁、短文件名补丁和压缩兜底数据。

旧版以 `manifest.ini`、`rules.ini` 和 `StaticHash_Input` 文本候选为主。新版不再兼容旧草稿格式，正式发布扩展集目录只保留 `manifest.int` 与 `rules.int` 两个文件。

## 极简规则包

早期草稿扩展集会把大量资源路径、目录名和文件名直接写入文本文件，体积可能达到数百 KB。新版发布器改成“规则优先，压缩兜底”的路线：

1. 优先推导 VoicePattern、背景、立绘、系统图等可枚举命名规律。
2. 对能由规则表达的资源，不再保存完整路径。
3. 对目录结构简单、语音规则覆盖大的游戏，使用 `minimal-voice-rules` 极简包。
4. 对规则无法可靠覆盖的部分，使用二进制前缀压缩与 LZSS 压缩作为兜底。
5. 对只有文件名 Hash 缺口的情况，可以只保存短文件名候选，不回填完整资源路径。

为了保证成功率，极简包不是无条件启用。当前策略是：VoicePattern 覆盖超过 45%，且额外短文件名补丁不超过资源数 35% 时，才启用极简包；否则自动回到压缩兜底包。

样本结果：

```text
Orthros / ツヴァイトリガー
  manifest.int: 273 B
  rules.int:    8106 B
  roundtrip:    90.31%

Purple / 何度目かのはじめまして
  manifest.int: 245 B
  rules.int:    11780 B
  roundtrip:    88.03%
```

这条路线的目标是：在不牺牲成功率的前提下，把正式扩展集从“大候选文本包”压回“几 KB 到十几 KB 的规则包”。

## 发布者流程

扩展集制作者通常按下面的流程工作：

1. 先用功能 3 提取资源并收集动态 Hash。
2. 根据 `Extractor_Output` + `StringHashDumper_Output` 生成 `Restored_Extractor_Output`。
3. 在“制作扩展集”页选择目标游戏，生成最小扩展集草稿。
4. 编辑扩展集信息：会社、游戏名、制作者、版本、说明。
5. 选择或确认测试来源目录。
6. 使用 C++ 原生测试窗口测试当前扩展集。
7. 测试通过后，安装到内置 `Extensions` 扩展集库，或单独发布扩展集目录。

制作目录固定在游戏目录下：

```text
游戏目录\
  Publisher\
    ExtensionDraft\
      manifest.int
      rules.int
```

用户工作目录固定在游戏目录下：

```text
游戏目录\
  User\
    1\
    2\
    3\
```

其中 `User\1` 对应功能 1，`User\2` 对应功能 2，`User\3` 对应传统动态模式。

## 正式发布目录

打包后的正式程序目录大致如下：

```text
CxdecV2ExtractorWorkbench\
  CxdecV2ExtractorWorkbench.exe
  README.md
  RELEASE_LAYOUT.md
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

正式版已经把 Python 和 PySide6 运行时打进 `_runtime`。使用者不需要自己安装 Python，也不要只复制单独的 exe；必须保留整个目录结构。

## 从源码运行

开发环境示例：

```powershell
cd D:\cxdecExtrator.V3.4\new2\CxdecV2ExtractorWorkbench_right_intro_cards_added
python .\cxdec_gui_pyside.py
```

如果 PySide6 不在默认 Python 环境里，请先安装依赖，或把当前环境中的 PySide6 加入 `PYTHONPATH`：

```powershell
python -m pip install -r requirements.txt
python .\cxdec_gui_pyside.py
```

依赖：

```text
PySide6
```

## 编译 C++ 核心

C++ 核心项目位于：

```text
new\CxdecCoreCLI\
```

使用 Visual Studio 2022 / MSBuild 编译：

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
  "D:\cxdecExtrator.V3.4\new\CxdecCoreCLI\CxdecCoreCLI.vcxproj" `
  /p:Configuration=Release `
  /p:Platform=Win32 `
  /m
```

编译出的核心程序：

```text
new\CxdecCoreCLI\Release\CxdecCoreCLI.exe
```

工作台正式运行时会使用：

```text
core\CxdecCoreCLI.exe
```

## 打包正式程序

工作台目录下提供 `build_release.ps1`，用于通过 PyInstaller 生成 onedir 正式版：

```powershell
cd D:\cxdecExtrator.V3.4\new2\CxdecV2ExtractorWorkbench_right_intro_cards_added
.\build_release.ps1
```

生成结果位于：

```text
release\CxdecV2ExtractorWorkbench\
```

## 注意事项

- 本工具面向资源研究、归档和个人学习场景。
- 请只处理你有权处理的游戏文件。
- 动态模块需要目标游戏进程配合运行，杀毒软件或系统权限可能影响注入行为。
- 扩展集不应记录制作者本机的完整绝对路径。
- `StaticHash_Output`、`Extractor_Output`、`StringHashDumper_Output`、`Restored_Extractor_Output` 都是运行时输出，不应作为 GitHub 正式发布扩展集内容。

## 当前状态

当前版本已经具备：

- 三种使用者主流程。
- C++ 原生核心操作入口。
- C++ 原生扩展集测试窗口。
- 新版 `manifest.int` / `rules.int` 扩展集格式。
- 极简规则包与压缩兜底包自动选择。
- 扩展集管理、制作、测试与安装流程。

后续可以继续增强的方向：

- 更丰富的背景、立绘、系统图命名规则推导。
- 更细粒度的扩展集测试报告。
- 更友好的扩展集编辑器。
- 更多会社样本和规则库。
