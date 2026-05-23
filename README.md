# Cx2bro
适用于krkrz 引擎，CxdecV2 (hxv4) 加密方式的 Galgame 解包工具。支持多种解包模式，还可制作并分享本游戏的Cx2bro扩展集，Cx2bro扩展集库越丰富，一键解包的通用性越强。 
- 👉[项目由来](https://github.com/zeli624233/Cx2bro/blob/main/项目由来.md)
- 👉[支持的游戏扩展集](https://github.com/zeli624233/Cx2bro/blob/main/支持的游戏.md)
## 首页（解包部分）
- 加载你的游戏的exe程序。
<pre>注意:
    1.不能有验证密钥，key，通行证之类的。
    2.还有不要把本工具和游戏本体放到c盘的目录。
    3.尽量使用英文目录，小概率会有不可预知的问题。</pre>
### 1. 该作扩展集提取
- 步骤0：选择选择该游戏的扩展集(如果扩展集库有的话)。
- 步骤1：根据该作扩展集生成静态hash表。
- 步骤2: 提取本作的动态XP3资源。
- 步骤3：用静态hash表还原资源名。
<pre>补充说明：
    1.选择该模式的解包的话，会自动生成一个解包目录位置：游戏exe目录\user\1
    2.按流程下来的话，会生成3个文件夹：
     1.Extractor_Output（存放解包后的hash文件）
     2.StaticHash_Output（存放根据扩展集生成的静态hash文件）
     3.Restored_Extractor_Output（还原的文件目录）
</pre>
### 2. 该会社集合撞新作
- 步骤0：选择你的游戏的会社（如果扩展集库有，且数量够多的话）。
- 步骤1：根据会社集合生成静态hash表。
- 步骤2: 提取本作的动态XP3资源。
- 步骤3：用静态hash表还原资源名。
<pre>补充说明：
    1.适合新作还没有专用扩展集，但同会社旧作命名规律可能复用的情况。（还原效果，取决于该社扩展库内的游戏数量）
    2.选择该模式的解包的话，会自动生成一个解包目录位置：游戏exe目录\user\2
    3.按流程下来的话，会生成3个文件夹：
     1.Extractor_Output（存放解包后的hash文件）
     2.StaticHash_Output（存放根据扩展集生成的静态hash文件）
     3.Restored_Extractor_Output（还原的文件目录）
    4.该模式还在研究中，由于扩展集数量还不足（人手不够），没有足够的样本来验证效果，所以目前图一乐。
</pre>


### 3. 传统动态模式
- 步骤1：提取本作的动态XP3资源。
- 步骤2：加载动态hash收集模块。
- 步骤3：用动态hash还原回文件名。
<pre>补充说明：
    1.该模式是保底模式，也是最麻烦的模式，如果前面前面两种模式都用不了，就用这种。
    2.步骤2的动态hash收集模块。本质就是让你进游戏从头到尾打一遍(可快进)，的同时，
      收集游戏运行时的hash的原文件名，收集到的hash资源越多（看hash收集面板），恢复的效果越好。
    3.hash收集模块有记忆的功能，如果在收集过程中游戏exe程序崩溃了，在次使用本模块打开游戏exe程序（一定要这样）
      他会记录已经收集的文件信息，进度，你读取崩溃前的游戏位置的存档，再继续玩下去即可。
    4.选择该模式的解包的话，会自动生成一个解包目录位置：游戏exe目录\user\3
    5.按流程下来的话，会生成3个文件夹：
     1.Extractor_Output（存放解包后的hash文件）
     2.StringHashDumper_Output（存放hash收集模块收集的内容）
     3.Restored_Extractor_Output（还原的文件目录）
</pre>
# 制作扩展集
- 步骤0：使用传统模式解包游戏的资源，并确保还原率不低于75%。
 <pre>（不要问为什么，我设定的，扩展集至少能还原大部分的资源，否则他的意义何在呢？（步骤3））</pre>
- 步骤1：生成当前游戏的扩展集。
- 步骤2：编辑当前扩展集信息。<pre>（可编辑的有：会社名，游戏名称，贡献者，版本，日期，扩展集说明。）</pre>
- 步骤3：测试当前扩展集<pre>（点一下，软件会自动验证，扩展集是否能用于游戏资源的还原，并统计出还原率，自动记录到扩展集里面）</pre>
- 步骤4（可选，校验用）：打开当前扩展集目录<pre>（这里有两个目录：游戏exe目录\Publisher\ExtensionDraft和游戏exe目录\Publisher\ExtensionDraft\会社名\游戏名，这里默认打开的是后者）</pre>
- 步骤5：安装到扩展集库。（点一下即可，右边会有成功提示的）
- 步骤6：打开当前扩展集库。（可选，校验用）
- 步骤7：到扩展集，查看扩展集是否安装上了，测试该作扩展集提取该游戏。（如果没问题，就完成了）
<pre>制作完后的扩展集由两个文件组成：manifest.int和rule.int，前者存储扩展集显示信息如会社名，游戏名等等。
     后者则存储规则+不可压缩信息，是还原游戏的核心依据，为了使得该文件足够小巧，里面是存储的是二进制数据。
     如果使用记事本等软件打开，容易发生报错，文件体积归零等问题。</pre>
# 扩展集
- 能查看当前软件所安装的扩展集数量及会社名，游戏名，当前扩展集的详细信息等等。
- 管理和测试当前游戏的扩展集，支持打开当前扩展集游戏目录或者会社目录。
  <pre> Cx2bro扩展集是Cxbro的核心功能，扩展集库越丰富，玩家解包游戏时就越方便，而不必费劲精力和时间用传统方式解包。
    但是扩展集库的数据并不能凭空生成，需要有人肯花时间，精力来制作。
    如果扩展集库没有该游戏，而你又跑了传统模式并成功还原了游戏的的资源的话，不妨再花2分钟的时间，制作本游戏的扩展集，下次就可以一键使用了，
    当然，你也可以自己收藏的同时，分享一份给我，让我加入到本项目的Github库中，增加本项目的游戏支持数量，从而帮助到更多的玩家，以及未来的你！
</pre>


# 关于
- 介绍版本信息，作者信息，鸣谢/许可证，还有帮助按钮。没什么好说的，自己看。

## 声明
本项目基于 [KrkrExtractForCxdecV3.3Extra_Plus](https://github.com/zeli624233/KrkrExtractForCxdecV3.3Extra_Plus) 重构开发。
本项目使用 **AGPL-3.0 license** 许可证开源，与所有上游项目保持一致。

### 项目历史

- 2023: **YeLikesss** 发布原始[KrkrExtractForCxdecV2](https://github.com/YeLikesss/KrkrExtractForCxdecV2)，实现CxdecV2基础解包与动态hash提取。
- 2024: **Kinotern** 发布[KrkrExtractForCxdecV3.3Extra](https://github.com/Kinotern/KrkrExtractForCxdecV3.3Extra)版本，添加原生Key提取和批量解包UI。
- 2026: **zeli624233** 发布[KrkrExtractForCxdecV3.3Extra_Plus](https://github.com/zeli624233/KrkrExtractForCxdecV3.3Extra_Plus)版本，添加hash文件名还原功能。
- 2026: **zeli624233** 基于Plus版本重构,并命名为[Cx2bro](https://github.com/zeli624233/Cx2bro)，添加扩展集系统，静态hash解析支持和跨作规则继承等功能。

### 特别感谢
- **YeLikesss** 大佬对CxdecV2基础解包与动态hash提取。
- **Kinotern** 大佬对完善Key提取模块与批量解包UI的完善。
- **YuriSizuku** 大佬对cxdecv2加密方式的文章分享。[哈希算法分析_以krkrz_hxv4](https://www.kungal.com/topic/3155)和[狠狠厥烂KiriKiriZ Cxdec拆包](https://linux.do/t/topic/940778)
- **GPT,Deepseek**对cxdecv2加密逆向分析，压缩算法建议等支持。
