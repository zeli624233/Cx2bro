# Cx2bro
适用于krkrz 引擎，CxdecV2 (hxv4) 加密方式的 Galgame 解包工具。支持多种解包模式，还可制作并分享本游戏的Cx2bro扩展集，Cx2bro扩展集库越丰富，一键解包的通用性越强。

## 项目由来

最近沉迷于Gal的解包研究，特别是我喜欢的游戏——オトメ世界の歩き方和何度目かのはじめまして的解包，发现他们都开始使用近几年新起的加密方式：cxdecv2（hxv4）for krkrz。
  他真的很坏，目录和文件居然都hash化了，游戏运行的时候才正常化，还难找的到他的hash转化方式(双重AES加密+全文件名哈希化)，使得很多工具对这种加密方式的游戏很难适配，也没有广泛性，通用性。
  作为一个菜鸟，我也没有什么好的方法去破解，毕竟每个游戏的AES加密都不一样，你破解也没用，下一个游戏又不适用了，当然你做汉化的另说，毕竟文件要全，否则没法做补丁。
  虽说如此，我还是找到了一个笨方法，在研究了各位大佬的Github库和对cxdecv2的加密文章分析后，发现可以反过来利用这样加密特性。把动态的hash文件值做静态hash化，而静态的hash文件又可再规则化+加一些必要数据，再配合一些压缩算法后，最后的文件体积最多就50KB而已，1000个游戏也才5MB，完全可以做成扩展集的形式，实现本项目对多游戏的支持，只要扩展集足够丰富！。
  当然这个过程还是很麻烦，复杂的，为此我要简化一下这个流程，做成一个工具，使得普通玩家和大神们也能参与进来这个项目，我相信，只要扩展集足够丰富，我们的工具支持的游戏就越多，广大gal玩家也能收益，反哺本项目。
</pre>





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
