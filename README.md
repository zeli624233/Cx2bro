<div align="center">

# Cx2bro

![version](https://img.shields.io/badge/version-v1.2.0-007ec6?labelColor=333333)
![platform](https://img.shields.io/badge/platform-Windows-007ec6?labelColor=333333&logo=windows&logoColor=white)
![license](https://img.shields.io/badge/license-AGPL--3.0-d93f0b?labelColor=333333)
![status](https://img.shields.io/badge/status-release-28a745?labelColor=333333)

</div>

适用于 krkrz 引擎、CxdecV2 (hxv4) 加密方式的 Galgame 解包工具。支持多种解包模式，可制作并分享游戏的 Cx2bro 扩展集，不到40KB占用，即可解包你喜欢的游戏！

## 靓点

- 该作扩展集提取：一键解包已收录游戏，减少重复采集
- 该会社合集撞新作：利用同会社扩展集规则，或多或少能解包新作
- 传统动态模式：传统人工采集，保底使用，如果以上两种方法用不了的话
- 扩展集社区生态：扩展集支持在线更新，只要社区的样本够多，扩展集就越多，还原大家贡献！。

## 资产

👉 [支持游戏列表(Cx2bro扩展集)](https://github.com/zeli624233/Cx2bro-extensions)（不定期更新！欢迎提交 Pull Request！）
👉 项目原理：[父模块](docs/parent-module.md) | [子模块](docs/child-module.md)

---

## 支持一下

如果 Cx2bro 确实帮到了你，麻烦给本项目一个 ⭐Star 吧！这是对开发者最好的赞誉！

## 快速开始

### 前置操作

加载游戏的 `.exe` 主程序

> ⚠️ 重要注意事项
> - 游戏不能包含密钥验证、通行证、加密校验类限制
> - 请勿将工具与游戏放置在C盘目录下
> - 建议使用全英文路径，规避未知异常问题

### 1. 该作扩展集提取（推荐）

选择已收录游戏扩展集 → 生成专属静态 Hash 表 → 提取 XP3 资源 → 静态哈希还原文件名

> 💡 补充说明
> - 输出目录：`游戏exe目录\user\1`
> - 自动生成目录：
>   - Extractor_Output：哈希原始资源文件
>   - StaticHash_Output：静态哈希规则数据
>   - Restored_Extractor_Output：最终还原成品资源

### 2. 该会社集合撞新作（实验性功能）

选择游戏所属会社合集 → 生成通用静态 Hash 表 → 提取 XP3 资源 → 批量还原文件名

> 💡 补充说明
> - 实验性功能，样本不足会影响还原效果
> - 输出目录：`游戏exe目录\user\2`
> - 目录结构与专属扩展集模式完全一致

### 3. 传统动态模式（兜底方案）

提取 XP3 原始资源 → 启动动态哈希采集模块 → 运行时映射还原文件名

> 💡 补充说明
> - 前两种模式失效时的最终兼容方案
> - 运行游戏采集映射，内容越全还原越完美，支持快进
> - 支持断点续存，崩溃重启可继承进度
> - 输出目录：`游戏exe目录\user\3`
> - 自动生成目录：
>   - Extractor_Output：哈希格式原始文件
>   - StringHashDumper_Output：动态采集哈希映射
>   - Restored_Extractor_Output：最终还原资源

## 扩展集系统

扩展集为本工具核心功能，制作流程简易快捷。

### 制作流程

1. 动态解包(还原率≥70%)
2. 生成专属扩展集
3. 编辑扩展集信息
4. 自动校验可用性
5. （可选）查看草稿文件
6. 安装至本地资源库
7. （可选）校验安装状态
8. 复测功能完成制作

> 设定标准：扩展集需保证大部分资源正常还原，具备实际使用价值

### 扩展集文件说明

成品包含两份核心文件：
- `manifest.int`：存储厂商、游戏、作者等展示信息
- `rule.int`：二进制规则文件，保存哈希运算与解密数据，体积小巧

> ⚠️ 警告：不能用记事本直接编辑，避免文件损坏、数据丢失

### 扩展集管理

- 统计扩展集总量、厂商归属、游戏信息与详细参数
- 支持扩展集可用性测试、资源管理、本地目录快速跳转

> 💡 社区寄语
> 
> 丰富的扩展集库可以大幅简化解包流程，免去繁琐的动态采集步骤。
> 扩展集无法自动生成，需要玩家手动制作维护。
> 如果你已成功通过传统模式解包某款游戏，只需花费两分钟制作专属扩展集，
> 既可自用一键解包，也可提交仓库收录，持续丰富社区资源、帮助更多同好。

## 许可证

本项目基于 KrkrExtractForCxdecV3.3Extra_Plus 重构开发，遵循 GNU AGPL-3.0 开源协议，与上游项目许可证保持统一。

## 项目历史

- 2023：YeLikesss 发布 KrkrExtractForCxdecV2，实现CxdecV2基础解包与动态哈希提取
- 2024：Kinotern 发布 KrkrExtractForCxdecV3.3Extra，新增原生密钥提取与批量解包界面
- 2026：zeli624233 发布 KrkrExtractForCxdecV3.3Extra_Plus，新增哈希文件名还原能力
- 2026：zeli624233 基于Plus版本整体重构，定名 Cx2bro，新增扩展集系统、静态哈希解析等功能

## 特别感谢

- YeLikesss：CxdecV2基础解包与动态哈希技术奠基
- Kinotern：密钥提取模块、批量解包界面优化完善
- YuriSizuku：CxdecV2加密原理、哈希算法相关技术文章分享
- GPT、Deepseek：提供加密逆向思路与压缩算法优化建议
