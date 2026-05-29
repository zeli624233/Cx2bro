# Cx2bro

[![Version](https://img.shields.io/github/v/release/zeli624233/Cx2bro?color=blue&label=version)](https://github.com/zeli624233/Cx2bro/releases)
[![Platform](https://img.shields.io/badge/platform-Windows-lightgrey.svg)](https://github.com/zeli624233/Cx2bro/releases)
[![License](https://img.shields.io/badge/license-AGPL--3.0-orange.svg)](https://github.com/zeli624233/Cx2bro/blob/main/LICENSE)
[![Size](https://img.shields.io/badge/size-%3C40KB-lightgrey.svg)](https://github.com/zeli624233/Cx2bro/releases/latest)

适用于 krkrz 引擎、CxdecV2 (hxv4) 加密方式的 Galgame 解包工具。支持多种解包模式，可制作并分享游戏的 Cx2bro 扩展集，不到40KB占用，即可解包你喜欢的游戏，快来试一下！

## 靓点

- 该作扩展集提取：一键解包已收录游戏，无需动态采集
- 该会社合集撞新作：利用同会社扩展集规则，解包未收录新作
- 传统动态模式：DLL注入捕获文件名与哈希，制作专属扩展集
- 扩展集社区生态：可制作、分享、复用扩展集，降低重复采集成本

## 资产

👉 [支持游戏列表(Cx2bro扩展集)](https://github.com/zeli624233/Cx2bro-extensions)（不定期更新！欢迎提交 Pull Request！）
👉 项目原理：[父模块](docs/parent-module.md) | [子模块](docs/child-module.md)

---

## 支持一下

如果 Cx2bro 确实帮到了你，麻烦给本项目一个 ⭐Star 吧！这是对开发者最好的赞誉！
## 快速上手：
### 前置操作
加载游戏 .exe 主程序
### ⚠️ 注意
游戏不能有密钥验证、通行证等加密限制
工具与游戏请勿放在 C 盘
建议使用全英文路径
### 1. 该作扩展集提取（推荐）
选择对应游戏扩展集 → 生成静态 Hash 表 → 提取并还原资源
💡 输出：游戏目录\user\1，自动生成原始资源、规则数据、最终成品三个目录
### 2. 该会社合集撞新作（实验性）
选择对应会社合集 → 生成通用 Hash 表 → 批量还原资源
💡 输出：游戏目录\user\2，目录结构同模式 1；样本不足会影响还原效果
### 3. 传统动态模式（兜底）
提取原始资源 → 启动动态采集 → 运行游戏映射还原文件名
💡 输出：游戏目录\user\3；支持快进、断点续存，内容越全还原越好
### 4. 扩展集系统
制作流程
动态解包 (还原率≥70%) → 生成扩展集 → 编辑信息 → 校验可用性 → 安装至本地 → 复测完成
标准：保证大部分资源正常还原
文件说明
manifest.int：厂商、游戏、作者等信息
rule.int：哈希运算与解密数据（禁止用记事本编辑）
### 5. 管理功能
统计扩展集总量与信息
可用性测试、资源管理、快速跳转
💡 社区寄语
扩展集无法自动生成，需要玩家手动维护。如果你成功解包了某款游戏，花两分钟制作扩展集，既能自用一键解包，也能分享帮助更多同好。

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
