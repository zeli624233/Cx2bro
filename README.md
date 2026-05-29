<div align="center">

# ![image](https://github.com/zeli624233/Cx2bro/blob/main/logo.png)
![version](https://img.shields.io/badge/version-v1.2.0-007ec6?labelColor=333333)
![platform](https://img.shields.io/badge/platform-Windows-007ec6?labelColor=333333&logo=windows&logoColor=white)
![license](https://img.shields.io/badge/license-AGPL--3.0-d93f0b?labelColor=333333)
![status](https://img.shields.io/badge/status-release-28a745?labelColor=333333)

</div>

适用于 krkrz 引擎、CxdecV2 (hxv4) 加密方式的 Galgame 解包工具。支持多种解包模式，可制作并分享游戏的 Cx2bro 扩展集，不到40KB占用，即可解包游戏！

## 靓点

- 该作扩展集提取：一键解包已收录游戏，无需动态采集。
- 该会社合集撞新作：利用同会社扩展集规则，解包未收录新作。
- 传统动态模式：DLL注入捕获文件名与哈希，制作专属扩展集。
- 扩展集社区生态：可制作、分享、复用扩展集，降低重复采集成本。

## 资产

- 👉 [支持游戏列表(Cx2bro扩展集)](https://github.com/zeli624233/Cx2bro-extensions)（不定期更新！欢迎提交 Pull Request！）
## ⭐支持一下！
- 如果 Cx2bro 确实帮到了你，麻烦给本项目一个 ⭐Star 吧！这是对开发者最好的赞誉！
---
## 快速上手：
### 前置操作
加载游戏 .exe 主程序。
### ⚠️ 注意
游戏不能有密钥验证、通行证等加密限制。
工具与游戏请勿放在 C 盘。
建议使用全英文路径。
### 1. 该作扩展集提取（推荐）
- 选择对应游戏扩展集 → 生成静态 Hash 表 → 提取并还原资源。
- 💡 输出：游戏目录\user\1
### 2. 该会社合集撞新作（实验性）
- 选择对应会社合集 → 生成通用 Hash 表 → 批量还原资源。
- 💡 输出：游戏目录\user\2，
- `样本不足会影响还原效果。`
### 3. 传统动态模式（兜底）
- 提取原始资源 → 启动动态采集 → 提取并还原资源。
- 💡 输出：游戏目录\user\3；
### 4. 扩展集制作
- 传统解包→ 生成扩展集 → 编辑信息 → 校验 → 安装至本地。
- 文件说明：
- manifest.int：厂商、游戏、作者等信息。
- rule.int：哈希运算与解密数据。
### 5. 管理功能
- 统计扩展集总量与信息。
- 在线更新、资源管理、快速跳转。
---
## AGPL-3.0 License
本项目基于 KrkrExtractForCxdecV3.3Extra_Plus 重构开发， 遵循 GNU AGPL-3.0 开源协议，与上游项目许可证保持统一。
## 项目历史

- 2023：YeLikesss 发布 KrkrExtractForCxdecV2，实现CxdecV2基础解包与动态哈希提取。
- 2024：Kinotern 发布 KrkrExtractForCxdecV3.3Extra，新增原生密钥提取与批量解包界面。
- 2026：zeli624233 发布 KrkrExtractForCxdecV3.3Extra_Plus，新增哈希文件名还原能力。
- 2026：zeli624233 基于Plus版本整体重构，定名 Cx2bro，新增扩展集系统、静态哈希解析等功能。

## 特别感谢

- YeLikesss：CxdecV2基础解包与动态哈希技术奠基。
- Kinotern：密钥提取模块、批量解包界面优化完善。
- YuriSizuku：CxdecV2加密原理、哈希算法相关技术文章分享。
- GPT、Deepseek：提供加密逆向思路与压缩算法优化建议。
