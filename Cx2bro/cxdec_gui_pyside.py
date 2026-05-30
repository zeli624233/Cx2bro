from __future__ import annotations

import calendar
import ctypes
import configparser
import html
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from datetime import date, datetime
from dataclasses import dataclass
from pathlib import Path

from PySide6.QtCore import QProcess, QTimer, Qt
from PySide6.QtGui import QBrush, QColor, QIcon, QPixmap
from PySide6.QtWidgets import (
    QAbstractItemView,
    QApplication,
    QCheckBox,
    QDialog,
    QFileDialog,
    QGridLayout,
    QGroupBox,
    QHeaderView,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMainWindow,
    QMessageBox,
    QProgressBar,
    QPushButton,
    QSizePolicy,
    QPlainTextEdit,
    QSpinBox,
    QTableWidget,
    QTableWidgetItem,
    QTextBrowser,
    QStackedWidget,
    QTabBar,
    QVBoxLayout,
    QWidget,
)

from version import APP_VERSION


APP_TITLE = "Cx2bro"

# 首页主流程/辅助操作按钮尺寸：只调整按钮自身高度和行距，
# 不改窗口长宽，也不压缩“扩展集选择”区域。
HOME_STEP_BUTTON_HEIGHT = 34
HOME_AUX_BUTTON_HEIGHT = 34
# 首页下方“主流程 / 辅助操作”只在已有方框内放宽按钮间距，
# 不改变主窗口长宽，也不压缩“扩展集选择”区域。
HOME_FLOW_ROW_SPACING = 8
HOME_AUX_ROW_SPACING = 8
HOME_FLOW_GROUP_SPACING = 8


def app_version_label() -> str:
    """Return a display-safe version label with exactly one leading v."""
    raw = str(APP_VERSION).strip()
    return raw if raw.lower().startswith("v") else f"v{raw}"


def today_text() -> str:
    return date.today().strftime("%Y-%m-%d")


def parse_metadata_date(value: str) -> tuple[int, int, int]:
    """Parse a metadata date and clamp it to the user-facing limits."""
    text = (value or "").strip()
    match = re.search(r"(\d{1,4})\D+(\d{1,2})\D+(\d{1,2})", text)
    if not match:
        match = re.fullmatch(r"(\d{4})(\d{2})(\d{2})", text)
    if match:
        year, month, day = (int(match.group(1)), int(match.group(2)), int(match.group(3)))
    else:
        year, month, day = (int(part) for part in today_text().split("-"))
    year = max(2000, min(2100, year))
    month = max(1, min(12, month))
    max_day = calendar.monthrange(year, month)[1]
    day = max(1, min(max_day, day))
    return year, month, day


def normalize_metadata_date(value: str) -> str:
    year, month, day = parse_metadata_date(value)
    return f"{year:04d}-{month:02d}-{day:02d}"


USER_FLOW_DEFINITIONS: dict[int, dict[str, str]] = {
    1: {
        "name": "该作扩展集提取",
        "target": "适用于已经有当前游戏专用扩展集的情况。",
        "steps": (
            "1. 选择目标游戏主程序。\n"
            "2. 在扩展集选择中选中该会社和该作扩展集。\n"
            "3. 根据该作扩展集生成 User\\1\\StaticHash_Output。\n"
            "4. 提取动态 XP3 资源，输出到 User\\1\\Extractor_Output。\n"
            "5. 用 StaticHash_Output 静态还原资源名，生成 User\\1\\Restored_Extractor_Output 和还原报告。"
        ),
        "output": "输出目录：User\\1\\StaticHash_Output、User\\1\\Extractor_Output、User\\1\\Restored_Extractor_Output。",
        "next": "核心是“该作专用扩展集 + 动态 XP3 输出”的静态还原流程。",
    },
    2: {
        "name": "该会社集合撞新作",
        "target": "适用于新作没有专用扩展集，但同会社旧作命名规律可复用的情况。",
        "steps": (
            "1. 选择目标游戏主程序。\n"
            "2. 在扩展集选择中选中会社。\n"
            "3. 根据会社集合生成更大的 User\\2\\StaticHash_Output。\n"
            "4. 提取动态 XP3 资源，输出到 User\\2\\Extractor_Output。\n"
            "5. 用会社集合 Hash 去撞新作资源名，生成 User\\2\\Restored_Extractor_Output 和还原报告。"
        ),
        "output": "输出目录：User\\2\\StaticHash_Output、User\\2\\Extractor_Output、User\\2\\Restored_Extractor_Output。",
        "next": "核心依据是同一会社多部作品可能复用目录结构、资源命名习惯和素材分类。",
    },
    3: {
        "name": "传统动态模式",
        "target": "适用于功能 1/2 都不适用、没有可靠扩展集或还原率不够时的兜底流程。",
        "steps": (
            "1. 提取动态 XP3 资源，输出到 User\\3\\Extractor_Output。\n"
            "2. 加载动态 Hash 提取模块，进游戏跑 Hash，输出到 User\\3\\StringHashDumper_Output。\n"
            "3. 根据 Extractor_Output + StringHashDumper_Output 还原资源名。\n"
            "4. 生成 User\\3\\Restored_Extractor_Output，并查看还原成功率和报告。"
        ),
        "output": "输出目录：User\\3\\Extractor_Output、User\\3\\StringHashDumper_Output、User\\3\\Restored_Extractor_Output。",
        "next": "这是完整的传统流程，不是散装按钮；重点是先跑动态输出，再用动态 Hash 还原。",
    },
}

HELP_ITEMS: dict[str, list[tuple[str, str]]] = {
    "首页": [
        ("目标游戏", "选择当前要处理的游戏主程序 exe。首页和制作扩展集页共用同一个目标游戏路径，切换页面后不会丢失。"),
        ("功能 1：该作扩展集提取", "适合已经有当前游戏专用扩展集的情况。流程是：先根据该作扩展集生成 User\\1\\StaticHash_Output，再提取动态 XP3 到 User\\1\\Extractor_Output，最后用静态 Hash 还原到 User\\1\\Restored_Extractor_Output。"),
        ("功能 2：该会社集合撞新作", "适合新作还没有专用扩展集，但同会社旧作命名规律可能复用的情况。选择会社后，程序会合并该会社已搭载扩展集生成 User\\2\\StaticHash_Output，再用于撞新作资源名。"),
        ("功能 3：传统动态模式", "适合没有可靠扩展集，或功能 1/2 还原率不够时使用。流程是：提取 User\\3\\Extractor_Output，启动游戏后通过 Dumper 动态收集字符串 Hash 生成 StringHashDumper_Output，再用动态 Hash 还原资源名。收集器界面实时显示 DirectoryHash/FileNameHash 的收集进度、日志体积及预估、写入速度、运行时间，支持清空目录文件重新收集。"),
        ("流程概览", "右侧流程概览现在是当前任务仪表盘，只显示当前模式、目标游戏、扩展集状态、流程进度、还原结果、下一步建议和关键目录，不再放重复的功能说明。"),
        ("详细日志", "详细日志只记录真正执行过的动作和核心结果，例如生成 Hash、动态提取、还原、生成扩展集、测试扩展集、安装扩展集和错误信息。普通页面切换不会再刷屏。"),
        ("辅助操作", "检查状态会重新读取当前 User\\N 工作区；打开工作区用于查看过程文件；清理目录会清空当前功能的工作区；打开还原目录和查看还原报告用于检查最终结果。"),
        ("高级操作", "传统动态模式下可以打开 CxdecExtractorLoader 原版功能面板，用于动态 XP3 提取、动态字符串 Hash 收集、Key 提取、资源名还原和静态 Hash 映射生成等底层操作。"),
    ],
    "制作扩展集": [
        ("生成当前游戏扩展集", "根据当前目标游戏和已还原资源结果生成 Publisher\\ExtensionDraft 下的扩展集草稿。优先生成可复用规则；无法稳定推导规则时，再保留必要的 StaticHash_Input 内容。"),
        ("编辑当前扩展集信息", "编辑扩展集的会社、作品名、贡献者、版本、日期和说明。日期由年、月、日三个输入框组成，保存时统一为 xxxx-xx-xx；年份限制 2000-2100，月份 1-12，日期 1-31，个位数会自动补 0。"),
        ("测试当前扩展集", "用当前草稿和测试来源目录验证扩展集效果，右侧制作状态会显示测试来源、通过情况、成功率、问题和下一步建议。"),
        ("目录", "打开当前扩展集目录用于检查草稿文件；安装到扩展集库会把草稿写入 Extensions\\会社\\游戏；打开扩展集库目录用于查看已搭载的扩展集。"),
        ("自定义来源目录", "选择扩展集来源目录用于指定生成扩展集时参考的来源；选择测试来源目录用于指定测试当前扩展集时读取的 User 工作区或还原结果。"),
        ("制作状态", "右侧制作状态固定显示当前扩展集信息、manifest.int/rules.int、测试状态、安装位置和下一步建议，不再因为点击左侧按钮而大幅跳动。"),
    ],
    "扩展集": [
        ("编号列表", "会社列表和作品列表会显示 1.、2. 这样的编号。编号只用于界面阅读，内部仍使用原始会社名和作品名，不会影响选择、校验或安装。"),
        ("扩展集详情", "选中扩展集后，右侧会显示会社、作品、贡献者、版本、日期、说明、路径、HashSeed，以及 rules.int、manifest.int、测试状态等内容。"),
        ("刷新列表", "重新扫描 Extensions 目录。手动复制、删除或安装扩展集后，可以用它刷新界面。"),
        ("打开目录", "打开扩展集根目录或打开当前选中的会社/作品扩展集目录，方便手动检查配置文件和规则文件。"),
        ("在线更新", "检查 GitHub 仓库中是否有新的扩展集。检测到新增扩展集后，勾选并下载即可自动安装到 Extensions 目录。"),
        ("运行环境自检", "检查 core 目录、核心 EXE/DLL、CLI 可用性、Extensions 目录和扩展集数量。启动异常、按钮无反应或核心调用失败时，优先看这里。"),
        ("扩展集留言", "查看扩展集的用途、推荐目录结构，以及该作扩展集和会社集合扩展集在首页功能中的区别。"),
    ],
    "关于": [
        ("使用帮助", "打开当前三栏帮助窗口。具体功能说明都集中在这里，帮助窗口、首页右侧和关于窗口之间不再重复显示长篇说明。"),
        ("版本信息", "显示软件名称 Cx2bro、版本号 v1.3.0、运行平台和项目主页。核心用途、主要能力列表也在这里。"),
        ("作者信息", "维护者 ユイ可愛ね / zeli624233，GitHub 主页和 Cx2bro 项目仓库链接，当前项目主要改进方向以及使用说明。"),
        ("鸣谢 / 许可证", "底层 C++ 代码来源（YeLikesss/KrkrExtractForCxdecV2）、逆向分析参考、AGPL-3.0 许可证说明和免责声明。"),
    ],
}


class OnlineUpdateDialog(QDialog):
    """扩展集在线更新对话框。

    通过 GitHub Tree API 获取远程扩展集目录结构，
    与本地 Extensions/ 对比，检测新增扩展集，
    用户勾选后下载到本地。
    """

    INDEX_URL = "https://raw.githubusercontent.com/zeli624233/Cx2bro-Extensions/main/EXTENSIONS_INDEX.txt"
    RAW_BASE = "https://raw.githubusercontent.com/zeli624233/Cx2bro-Extensions/main/Extensions"

    def __init__(self, parent: "WorkbenchWindow") -> None:
        super().__init__(parent)
        self.workbench = parent
        self.setWindowTitle("Cx2bro - 扩展集在线更新")
        self.resize(880, 540)
        self.setMinimumSize(780, 420)
        self.setStyleSheet(self.styleSheet() + """
            QCheckBox::indicator:checked {
                background-color: #4CAF50;
                border: 1px solid #388E3C;
                border-radius: 3px;
            }
        """)
        if parent.paths.app_icon.exists():
            self.setWindowIcon(QIcon(str(parent.paths.app_icon)))

        self.new_checkboxes: list[tuple[str, str, QCheckBox]] = []  # (brand, game, checkbox)
        self.extensions_dir = parent.paths.extensions_dir
        self._highlighted_data_row = -1

        layout = QVBoxLayout(self)
        layout.setContentsMargins(12, 10, 12, 10)
        layout.setSpacing(8)

        self.status_label = QLabel("正在检查更新…")
        self.status_label.setWordWrap(True)
        layout.addWidget(self.status_label)

        self.progress_bar = QProgressBar()
        self.progress_bar.setVisible(False)
        self.progress_bar.setRange(0, 100)
        self.progress_bar.setValue(0)
        self.progress_bar.setStyleSheet("""
            QProgressBar::chunk {
                background-color: #4CAF50;
            }
        """)
        layout.addWidget(self.progress_bar)

        self.table = QTableWidget()
        self.table.setColumnCount(9)
        self.table.setHorizontalHeaderLabels(["入库编号", "会社", "作品", "发售日", "入库日", "解包率", "体积", "贡献者", "备注"])
        self.table.horizontalHeader().setStretchLastSection(True)
        self.table.verticalHeader().setVisible(False)
        self.table.setEditTriggers(QAbstractItemView.NoEditTriggers)
        self.table.setSelectionMode(QAbstractItemView.NoSelection)
        self.table.setFocusPolicy(Qt.NoFocus)
        self.table.cellClicked.connect(self._on_data_row_clicked)
        self.table.setShowGrid(False)
        layout.addWidget(self.table, 1)

        button_row = QHBoxLayout()
        self.repo_button = QPushButton("访问GitHub仓库")
        self.repo_button.clicked.connect(self.open_repo)
        button_row.addWidget(self.repo_button)
        self.local_repo_button = QPushButton("打开本地仓库")
        self.local_repo_button.clicked.connect(self.open_local_extensions)
        button_row.addWidget(self.local_repo_button)
        self.download_btn = QPushButton("下载选中")
        self.download_btn.setEnabled(False)
        self.download_btn.clicked.connect(self.download_selected)
        button_row.addWidget(self.download_btn)
        close_btn = QPushButton("关闭")
        close_btn.clicked.connect(self.reject)
        button_row.addWidget(close_btn)
        self.clear_btn = QPushButton("清空当前仓库")
        self.clear_btn.setStyleSheet("QPushButton { color: #D32F2F; }")
        self.clear_btn.clicked.connect(self.clear_extensions)
        button_row.addWidget(self.clear_btn)
        # 右侧状态区：status_indicator + timing_label 贴右
        button_row.addStretch(1)
        right_area = QHBoxLayout()
        right_area.setSpacing(20)
        right_area.setContentsMargins(0, 0, 0, 0)
        self.status_indicator = QLabel("")
        self.status_indicator.setStyleSheet("color: #388E3C; font-weight: bold; font-size: 14px;")
        right_area.addWidget(self.status_indicator)
        self.timing_label = QLabel("")
        self.timing_label.setStyleSheet("color: #388E3C; font-weight: bold; font-size: 14px;")
        right_area.addWidget(self.timing_label)
        button_row.addLayout(right_area)
        layout.addLayout(button_row)

        # 延迟启动检查，确保对话框先渲染出来
        QTimer.singleShot(100, self.check_updates)

    def _on_data_row_clicked(self, row: int, col: int) -> None:
        """数据行点击高亮 - 排除 checkbox 行和 section 标题行。"""
        # 跳过 checkbox 行（有 cellWidget）
        if self.table.cellWidget(row, 0) is not None:
            return
        # 跳过 section 标题行（跨 9 列）
        if self.table.columnSpan(row, 0) == 9:
            return
        # 清除上一行高亮
        if self._highlighted_data_row >= 0:
            for c in range(self.table.columnCount()):
                prev = self.table.item(self._highlighted_data_row, c)
                if prev is not None:
                    prev.setBackground(QBrush())
        # 高亮当前行
        for c in range(self.table.columnCount()):
            item = self.table.item(row, c)
            if item is not None:
                item.setBackground(QColor("#C8E6C9"))
        self._highlighted_data_row = row

    def check_updates(self) -> None:
        """下载 EXTENSIONS_INDEX.txt，对比本地，显示结果。"""
        self.status_label.setText("正在获取远程扩展集列表…")
        import time as time_mod
        _fetch_start = time_mod.time()
        try:
            req = urllib.request.Request(self.INDEX_URL)
            req.add_header("User-Agent", "Cx2bro/1.0")
            with urllib.request.urlopen(req, timeout=15) as resp:
                index_text = resp.read().decode("utf-8")
            _elapsed = time_mod.time() - _fetch_start
            self.timing_label.setText(f"本次访问延时：{int(_elapsed * 1000)}ms")
        except (urllib.error.URLError, urllib.error.HTTPError, OSError) as exc:
            _elapsed = time_mod.time() - _fetch_start
            self.status_label.setText(f"获取失败：{exc}")
            self.timing_label.setText(f"本次访问延时：{int(_elapsed * 1000)}ms")
            self.repo_button.setVisible(True)
            return

        # 解析索引文件：每行格式 入库编号|会社|作品|发售日|入库日|解包率|体积|贡献者|备注
        # 忽略空行和 # 开头的注释行
        RemoteEntry = tuple[str, str, str, str, str, str, str, str, str]  # (idx, brand, game, release_date,入库日, rate, size_kb, contributor, note)
        remote_entries: list[RemoteEntry] = []
        remote_pairs: set[tuple[str, str]] = set()
        for raw_line in index_text.splitlines():
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split("|")
            idx = parts[0].strip()
            brand = parts[1].strip() if len(parts) > 1 else ""
            game = parts[2].strip() if len(parts) > 2 else ""
            release_date = parts[3].strip() if len(parts) > 3 else ""
            added_date = parts[4].strip() if len(parts) > 4 else ""
            rate = parts[5].strip() if len(parts) > 5 else ""
            size_kb = parts[6].strip() if len(parts) > 6 else ""
            contributor = parts[7].strip() if len(parts) > 7 else ""
            note = parts[8].strip() if len(parts) > 8 else ""
            if brand and game:
                remote_entries.append((idx, brand, game, release_date, added_date, rate, size_kb, contributor, note))
                remote_pairs.add((brand, game))

        # 扫描本地扩展集目录对
        local_pairs: set[tuple[str, str]] = set()
        if self.extensions_dir.exists():
            for brand_dir in self.extensions_dir.iterdir():
                if brand_dir.is_dir():
                    for game_dir in brand_dir.iterdir():
                        if game_dir.is_dir():
                            local_pairs.add((brand_dir.name, game_dir.name))

        # 计算差集：远程有但本地没有
        new_pairs = sorted(remote_pairs - local_pairs, key=lambda x: (x[0], x[1]))
        existing_pairs = sorted(remote_pairs & local_pairs, key=lambda x: (x[0], x[1]))

        # --- 用 QTableWidget 展示远程列表 ---
        self.table.setRowCount(0)
        self.new_checkboxes.clear()

        def add_section_row(text: str, *, bold: bool = False, center: bool = False, green: bool = False) -> None:
            """在 table 末尾加一个跨列标题行。"""
            row = self.table.rowCount()
            self.table.insertRow(row)
            self.table.setSpan(row, 0, 1, 9)
            item = QTableWidgetItem(text)
            item.setFlags(Qt.ItemIsEnabled)
            if bold:
                f = item.font()
                f.setBold(True)
                item.setFont(f)
            if center:
                item.setTextAlignment(Qt.AlignCenter)
            if green:
                item.setForeground(Qt.GlobalColor.darkGreen)
            self.table.setItem(row, 0, item)

        def add_data_row(values: list[str]) -> None:
            row = self.table.rowCount()
            self.table.insertRow(row)
            for col, val in enumerate(values):
                item = QTableWidgetItem(val)
                item.setFlags(Qt.ItemIsEnabled)
                # 入库编号、发售日、入库日 居中对齐
                if col in (0, 3, 4):
                    item.setTextAlignment(Qt.AlignCenter)
                self.table.setItem(row, col, item)

        def set_col_widths(widths: list[int]) -> None:
            header = self.table.horizontalHeader()
            for col, w in enumerate(widths):
                header.setSectionResizeMode(col, QHeaderView.Interactive)
                header.resizeSection(col, w)
            header.setStretchLastSection(True)

        set_col_widths([70, 65, 160, 85, 85, 60, 50, 80, 120])

        # 空仓库提前返回
        if not remote_pairs:
            self.status_label.setText("仓库暂无扩展集。")
            add_section_row("仓库暂无扩展集，请等待作者上传。", bold=True, center=True)
            return

        # 标题
        add_section_row("── 远程仓库扩展集列表 ──", bold=True, center=True)

        # 数据行
        for entry in remote_entries:
            idx, brand, game, release_date, added_date, rate, size_kb, contributor, note = entry
            add_data_row([
                idx,
                brand,
                game,
                release_date if release_date else "-",
                added_date if added_date else "-",
                rate if rate else "-",
                size_kb if size_kb else "-",
                contributor if contributor else "-",
                note if note else "",
            ])

        # --- 对比结果 ---
        empty_row = self.table.rowCount()
        self.table.insertRow(empty_row)
        self.table.setRowHeight(empty_row, 8)

        if not new_pairs:
            self.status_label.setText("")
            self.status_indicator.setText("☑ 所有扩展集本地已存在")
            self.download_btn.setEnabled(False)
            return

        self.status_label.setText(f"发现 {len(new_pairs)} 个新扩展集")
        self.status_indicator.setText("")
        self.download_btn.setEnabled(True)

        add_section_row(f"◆ 新增（{len(new_pairs)} 个）：", bold=True)

        for brand, game in new_pairs:
            row = self.table.rowCount()
            self.table.insertRow(row)
            cb = QCheckBox(f"{brand} / {game}")
            cb.setChecked(True)
            cb.setStyleSheet("QCheckBox::indicator:checked { background-color: #4CAF50; border: 1px solid #388E3C; border-radius: 3px; }")
            # 使用 QWidget 容器让 checkbox 左对齐
            container = QWidget()
            clayout = QHBoxLayout(container)
            clayout.setContentsMargins(4, 0, 0, 0)
            clayout.addWidget(cb)
            clayout.addStretch(1)
            self.table.setSpan(row, 0, 1, 9)
            self.table.setCellWidget(row, 0, container)
            # 手动高亮选中行（NoSelection 模式下通过 checkbox 状态控制背景色）
            def _update_highlight(checked):
                container.setStyleSheet(
                    ".QWidget { background-color: #C8E6C9; }" if checked else ""
                )
            cb.toggled.connect(_update_highlight)
            _update_highlight(cb.isChecked())
            self.new_checkboxes.append((brand, game, cb))

        if existing_pairs:
            add_section_row(f"✓ 已存在（{len(existing_pairs)} 个）：", bold=True)
            for brand, game in existing_pairs:
                add_data_row(["", brand, game, "", "", "", "", "", ""])

    def open_repo(self) -> None:
        """在浏览器中打开仓库首页。"""
        import webbrowser
        webbrowser.open("https://github.com/zeli624233/Cx2bro-Extensions")

    def open_local_extensions(self) -> None:
        """打开本地扩展集目录。"""
        self.workbench.open_path(self.extensions_dir)

    def clear_extensions(self) -> None:
        """清空当前仓库所有扩展集后刷新页面。"""
        import shutil
        if not self.extensions_dir.exists():
            self.check_updates()
            return
        reply = QMessageBox.question(
            self, "Cx2bro", "确认清空所有扩展集？\n此操作不可撤销！",
            QMessageBox.Yes | QMessageBox.No, QMessageBox.No
        )
        if reply != QMessageBox.Yes:
            return
        for brand_dir in self.extensions_dir.iterdir():
            if brand_dir.is_dir():
                shutil.rmtree(str(brand_dir), ignore_errors=True)
        self.status_label.setText("已清空所有扩展集，正在重新获取远程列表…")
        self.check_updates()

    def download_selected(self) -> None:
        """下载用户勾选的扩展集。"""
        selected = [(b, g) for b, g, cb in self.new_checkboxes if cb.isChecked()]
        if not selected:
            QMessageBox.information(self, "Cx2bro", "请至少勾选一个扩展集。")
            return

        total = len(selected)
        self.download_btn.setEnabled(False)
        self.progress_bar.setVisible(True)
        self.progress_bar.setValue(0)
        self.status_label.setText(f"正在下载 {total} 个扩展集…")

        success_count = 0
        for idx, (brand, game) in enumerate(selected):
            self.status_label.setText(f"正在下载 {brand}/{game}…")
            target_dir = self.extensions_dir / brand / game
            target_dir.mkdir(parents=True, exist_ok=True)

            ok = True
            for filename in ("manifest.int", "rules.int"):
                url = f"{self.RAW_BASE}/{urllib.request.quote(brand)}/{urllib.request.quote(game)}/{filename}"
                dest = target_dir / filename
                try:
                    file_req = urllib.request.Request(url)
                    file_req.add_header("User-Agent", "Cx2bro/1.0")
                    with urllib.request.urlopen(file_req, timeout=30) as resp:
                        data_bytes = resp.read()
                    if not data_bytes:
                        self.workbench.log(f"在线更新：{brand}/{game}/{filename} 为空，跳过")
                        continue
                    dest.write_bytes(data_bytes)
                except (urllib.error.URLError, urllib.error.HTTPError, OSError) as exc:
                    self.workbench.log(f"在线更新：下载 {brand}/{game}/{filename} 失败：{exc}")
                    ok = False
                if not ok:
                    break

            if ok:
                success_count += 1
                self.workbench.log(f"在线更新：已下载 {brand}/{game}")
            else:
                self.workbench.log(f"在线更新：{brand}/{game} 下载不完整")

            self.progress_bar.setValue(int((idx + 1) / total * 100))

        self.status_label.setText(f"完成：成功下载 {success_count}/{total} 个扩展集")
        self.workbench.reload_extensions()
        QApplication.beep()
        if success_count == total:
            QMessageBox.information(self, "Cx2bro", f"全部下载完成。\n{success_count}/{total} 个扩展集已安装。")
        else:
            QMessageBox.warning(self, "Cx2bro", f"部分下载失败。\n成功 {success_count}/{total} 个，详见日志。")

        self.accept()


class HelpDialog(QDialog):
    """三栏帮助窗口：板块、功能、说明。"""

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setWindowTitle("帮助")
        self.resize(760, 420)

        layout = QHBoxLayout(self)
        self.section_list = QListWidget()
        self.feature_list = QListWidget()
        self.detail = QPlainTextEdit()
        self.detail.setReadOnly(True)

        layout.addWidget(self.section_list, 1)
        layout.addWidget(self.feature_list, 2)
        layout.addWidget(self.detail, 3)

        self.section_list.addItems(HELP_ITEMS.keys())
        self.section_list.currentTextChanged.connect(self.on_section_selected)
        self.feature_list.currentRowChanged.connect(self.on_feature_selected)
        self.section_list.setCurrentRow(0)

    def on_section_selected(self, section: str) -> None:
        self.feature_list.clear()
        self.feature_list.addItems([name for name, _ in HELP_ITEMS.get(section, [])])
        if self.feature_list.count():
            self.feature_list.setCurrentRow(0)

    def on_feature_selected(self, row: int) -> None:
        section = self.section_list.currentItem().text() if self.section_list.currentItem() else ""
        items = HELP_ITEMS.get(section, [])
        if row < 0 or row >= len(items):
            self.detail.clear()
            return
        name, detail = items[row]
        self.detail.setPlainText(f"{section} / {name}\n\n{detail}")


class PublisherMetadataDialog(QDialog):
    def __init__(self, parent: "WorkbenchWindow", metadata: dict[str, str]) -> None:
        super().__init__(parent)
        self.setWindowTitle("编辑扩展集信息")
        self.setMinimumWidth(520)
        self.accepted_metadata: dict[str, str] | None = None

        layout = QVBoxLayout(self)
        form = QGridLayout()
        self.brand_edit = QLineEdit(metadata.get("brand", ""))
        self.game_edit = QLineEdit(metadata.get("game", ""))
        self.author_edit = QLineEdit(metadata.get("author", ""))
        self.version_edit = QLineEdit(metadata.get("version", "1.0"))
        self.summary_edit = QLineEdit(metadata.get("summary", "填你想填的！"))

        year, month, day = parse_metadata_date(metadata.get("date", ""))
        self.date_year_spin = QSpinBox()
        self.date_year_spin.setRange(2000, 2100)
        self.date_year_spin.setValue(year)
        self.date_month_spin = QSpinBox()
        self.date_month_spin.setRange(1, 12)
        self.date_month_spin.setValue(month)
        self.date_day_spin = QSpinBox()
        self.date_day_spin.setRange(1, 31)
        self.date_day_spin.setValue(day)
        date_row = QHBoxLayout()
        date_row.setContentsMargins(0, 0, 0, 0)
        date_row.addWidget(self.date_year_spin)
        date_row.addWidget(QLabel("年"))
        date_row.addWidget(self.date_month_spin)
        date_row.addWidget(QLabel("月"))
        date_row.addWidget(self.date_day_spin)
        date_row.addWidget(QLabel("日"))
        date_row.addStretch(1)
        date_widget = QWidget()
        date_widget.setLayout(date_row)

        form.addWidget(QLabel("会社"), 0, 0)
        form.addWidget(self.brand_edit, 0, 1)
        form.addWidget(QLabel("游戏名称"), 1, 0)
        form.addWidget(self.game_edit, 1, 1)
        form.addWidget(QLabel("贡献者"), 2, 0)
        form.addWidget(self.author_edit, 2, 1)
        form.addWidget(QLabel("版本"), 3, 0)
        form.addWidget(self.version_edit, 3, 1)
        form.addWidget(QLabel("日期"), 4, 0)
        form.addWidget(date_widget, 4, 1)
        form.addWidget(QLabel("扩展集留言"), 5, 0)
        form.addWidget(self.summary_edit, 5, 1)
        form.setColumnStretch(1, 1)
        layout.addLayout(form)

        buttons = QHBoxLayout()
        buttons.addStretch(1)
        ok_button = QPushButton("确定")
        cancel_button = QPushButton("取消")
        ok_button.clicked.connect(self.accept_metadata)
        cancel_button.clicked.connect(self.reject)
        buttons.addWidget(ok_button)
        buttons.addWidget(cancel_button)
        layout.addLayout(buttons)

    def accept_metadata(self) -> None:
        brand = self.brand_edit.text().strip()
        game = self.game_edit.text().strip()
        if not brand or not game:
            QMessageBox.information(self, "编辑扩展集信息", "会社和游戏名称不能为空。")
            return
        self.accepted_metadata = {
            "brand": brand,
            "game": game,
            "author": self.author_edit.text().strip(),
            "version": self.version_edit.text().strip() or "1.0",
            "date": f"{self.date_year_spin.value():04d}-{self.date_month_spin.value():02d}-{self.date_day_spin.value():02d}",
            "summary": self.summary_edit.text().strip(),
        }
        self.accept()


class LegacyLoaderDialog(QDialog):
    """旧版 CxdecExtractorLoader 功能面板。

    保留原版五个入口，方便熟悉旧工具的用户直接按旧名称操作；
    实际执行仍复用新版工作台里的 CLI 调用与 User\\N 工作区管理。
    """

    def __init__(self, parent: "WorkbenchWindow") -> None:
        super().__init__(parent)
        self.workbench = parent
        self.setWindowTitle("CxdecExtractorLoader")
        if parent.paths.app_icon.exists():
            self.setWindowIcon(QIcon(str(parent.paths.app_icon)))
        self.resize(456, 400)
        self.setMinimumSize(456, 400)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 26, 0, 26)
        layout.setSpacing(20)
        layout.addStretch(1)

        self._add_legacy_button(layout, "加载解包模块", "extract")
        self._add_legacy_button(layout, "加载字符串Hash提取模块", "stringhash")
        self._add_legacy_button(layout, "加载Key提取模块", "keydump")
        self._add_legacy_button(layout, "还原资源文件名", "restore")
        self._add_legacy_button(layout, "静态生成Hash映射", "static-hash")

        layout.addStretch(2)

    def _add_legacy_button(self, layout: QVBoxLayout, text: str, action: str) -> None:
        row = QHBoxLayout()
        row.addStretch(1)
        button = QPushButton(text)
        button.setObjectName("legacyLoaderButton")
        button.setFixedSize(190, 28)
        button.clicked.connect(lambda _checked=False, key=action: self.workbench.run_legacy_loader_action(key))
        row.addWidget(button)
        row.addStretch(1)
        layout.addLayout(row)


class AboutDialog(QDialog):
    """关于窗口：集中放帮助、版本、作者和鸣谢入口。"""

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.parent_window = parent
        self.setWindowTitle("关于")
        self.resize(760, 560)
        self.setMinimumSize(720, 520)

        if parent is not None and hasattr(parent, "paths") and parent.paths.app_icon.exists():
            self.setWindowIcon(QIcon(str(parent.paths.app_icon)))

        layout = QVBoxLayout(self)
        layout.setContentsMargins(14, 12, 14, 14)
        layout.setSpacing(10)

        header_group = QGroupBox()
        header_group.setObjectName("aboutHeaderFrame")
        header_layout = QHBoxLayout(header_group)
        header_layout.setContentsMargins(14, 10, 14, 10)
        header_layout.setSpacing(14)

        self.avatar_label = QLabel()
        self.avatar_label.setObjectName("aboutAvatar")
        self.avatar_label.setFixedSize(92, 92)
        self.avatar_label.setAlignment(Qt.AlignCenter)
        self._load_avatar()
        header_layout.addWidget(self.avatar_label, 0, Qt.AlignTop)

        header_text = QLabel(
            f"<div style='font-size:22px;font-weight:800;'>{html.escape(APP_TITLE)}</div>"
            f"<div style='font-size:14px;margin-top:3px;'><b>版本：</b>{html.escape(app_version_label())}</div>"
            "<div style='font-size:13px;color:#555;margin-top:6px;'>"
            "CxdecV2 / krkrz_hxv4 资源提取、Hash 命名还原与扩展集工作台"
            "</div>"
        )
        header_text.setTextFormat(Qt.RichText)
        header_text.setWordWrap(True)
        header_layout.addWidget(header_text, 1)
        layout.addWidget(header_group)

        option_row = QHBoxLayout()
        option_row.setSpacing(10)

        help_group = QGroupBox()
        help_group.setObjectName("aboutFrame")
        help_layout = QHBoxLayout(help_group)
        help_layout.setContentsMargins(10, 8, 10, 8)
        help_button = QPushButton("使用帮助")
        help_button.setAutoDefault(False)
        help_button.clicked.connect(self.open_help)
        help_layout.addWidget(help_button)
        option_row.addWidget(help_group, 1)

        info_group = QGroupBox()
        info_group.setObjectName("aboutFrame")
        grid = QGridLayout(info_group)
        grid.setContentsMargins(10, 8, 10, 8)
        grid.setHorizontalSpacing(6)
        grid.setVerticalSpacing(6)

        self.about_buttons: dict[str, QPushButton] = {}
        version_button = self._about_option_button("version", "版本信息", self.show_version)
        author_button = self._about_option_button("author", "作者信息", self.show_author)
        credit_button = self._about_option_button("credits", "鸣谢 / 许可证", self.show_credits)

        grid.addWidget(version_button, 0, 0)
        grid.addWidget(author_button, 0, 1)
        grid.addWidget(credit_button, 0, 2)
        for column in range(3):
            grid.setColumnStretch(column, 1)
        option_row.addWidget(info_group, 3)
        layout.addLayout(option_row)

        self.detail = QTextBrowser()
        self.detail.setObjectName("aboutDetail")
        self.detail.setReadOnly(True)
        self.detail.setOpenExternalLinks(True)
        layout.addWidget(self.detail, 1)
        self.show_version()

    def _load_avatar(self) -> None:
        avatar_path = None
        if self.parent_window is not None and hasattr(self.parent_window, "paths"):
            candidate = getattr(self.parent_window.paths, "about_avatar", None)
            if candidate is not None and candidate.exists():
                avatar_path = candidate
        if avatar_path is None:
            fallback = Path(__file__).resolve().parent / "about_avatar.jpg"
            if fallback.exists():
                avatar_path = fallback
        if avatar_path is not None:
            pixmap = QPixmap(str(avatar_path))
            if not pixmap.isNull():
                self.avatar_label.setPixmap(pixmap.scaled(88, 88, Qt.KeepAspectRatioByExpanding, Qt.SmoothTransformation))
                return
        self.avatar_label.setText("ユイ\n可愛ね")

    def _about_option_button(self, key: str, text: str, slot) -> QPushButton:
        button = QPushButton(text)
        button.setObjectName("aboutOptionButton")
        button.setCheckable(True)
        button.setAutoDefault(False)
        button.clicked.connect(slot)
        self.about_buttons[key] = button
        return button

    def _set_about_active(self, key: str) -> None:
        for name, button in self.about_buttons.items():
            button.setChecked(name == key)

    def _set_detail_html(self, title: str, subtitle: str, body: str) -> None:
        self.detail.setHtml(
            """
            <html>
            <head>
            <style>
                body { font-family: 'Microsoft YaHei UI', 'Microsoft YaHei', sans-serif; font-size: 13px; color: #202124; line-height: 1.55; }
                h1 { font-size: 22px; margin: 0 0 4px 0; font-weight: 800; }
                h2 { font-size: 17px; margin: 16px 0 8px 0; font-weight: 800; color: #111827; }
                h3 { font-size: 15px; margin: 12px 0 6px 0; font-weight: 800; }
                p { margin: 5px 0; }
                ul { margin-top: 6px; margin-bottom: 8px; }
                li { margin: 3px 0; }
                .subtitle { font-size: 13px; color: #555; margin-bottom: 10px; }
                .badge { display: inline-block; font-size: 12px; font-weight: 700; background: #eef3ff; color: #1f4e9e; padding: 2px 6px; border-radius: 6px; }
                .important { font-weight: 800; color: #9a3412; }
                .soft { color: #555; }
                .box { background: #f7f9fc; border: 1px solid #d7dde8; padding: 8px 10px; margin: 8px 0; }
                a { color: #0969da; text-decoration: none; }
                code { font-family: Consolas, 'Microsoft YaHei UI', monospace; background: #f1f3f5; padding: 1px 4px; }
            </style>
            </head>
            <body>
            """
            f"<h1>{html.escape(title)}</h1>"
            f"<div class='subtitle'>{html.escape(subtitle)}</div>"
            f"{body}"
            "</body></html>"
        )

    def open_help(self) -> None:
        if hasattr(self.parent_window, "show_help_dialog"):
            self.parent_window.show_help_dialog()

    def show_version(self) -> None:
        self._set_about_active("version")
        self._set_detail_html(
            "版本信息",
            "当前工作台版本与核心定位",
            f"""
            <p><span class='badge'>软件名称</span> <b>{html.escape(APP_TITLE)}</b></p>
            <p><span class='badge'>当前版本</span> <b>{html.escape(app_version_label())}</b></p>
            <p><span class='badge'>运行平台</span> Windows</p>
            <p><span class='badge'>项目主页</span> <a href='https://github.com/zeli624233/Cx2bro'>https://github.com/zeli624233/Cx2bro</a></p>

            <h2>核心用途</h2>
            <p><b>KrkrZ / CxdecV2 (hxv4)</b> 游戏解包 → hash 还原 → 扩展集制作复用，一条龙工作台。</p>
            <p>C++ 核心通过 <b>Detours DLL 注入</b> 复用游戏原生接口，<b>运行时特征码扫描</b> 自动定位 TVP/Cxdec 内部函数。</p>

            <h2>主要能力</h2>
            <ul>
                <li><b>该作扩展集提取：</b>按会社/作品选择扩展集，生成静态 Hash 再还原资源名。</li>
                <li><b>会社集合撞新作：</b>合并同会社旧作扩展集，拿已有命名规律去撞新作的 hash 文件名。</li>
                <li><b>传统动态模式：</b>无可靠扩展集时的兜底流程：动态 XP3 提取 → 动态 Hash 收集 → 还原。</li>
                <li><b>动态 Hash 收集器：</b>C++ 原生窗口，实时显示 DirectoryHash / FileNameHash 收集进度、日志体积、写入速度、运行时间。</li>
                <li><b>运行时 Hash 采集：</b>通过 VTable Hook 拦截游戏内 PathNameHasher / FileNameHasher，实时记录明文→Hash 映射。</li>
                <li><b>运行时密钥提取：</b>Detours Hook 游戏内 Hx/Cx/Verify 三个探针，提取加密密钥和 Garbro 分支顺序。</li>
                <li><b>XP3 批量提取：</b>多线程 Worker-Watchdog 架构，队列管理批量解包，支持拖拽和任务文件导入。</li>
                <li><b>文本自动解密：</b>支持 XOR 加密、位交换加密、zlib 压缩三种模式的文本资源解密。</li>
                <li><b>多线程并行还原：</b>支持工作线程数配置，含二次推理补充还原。</li>
                <li><b>扩展集制作：</b>自动推导语音规则和通用数字模式，生成 CXRI3 二进制规则包（LZSS 压缩）。</li>
                <li><b>扩展集在线更新：</b>从 GitHub 仓库检查并下载新增扩展集，自动安装到本地库。</li>
            </ul>

            <div class='box'>
                <b>这工具干什么的：</b>XP3 解出来全是 hash 文件名，根本看不出是什么。Cx2bro 就是干这个的——把 hash 文件名还原成人能看懂的名字，还能把还原规律做成扩展集，下次直接复用。
            </div>
            """,
        )

    def show_author(self) -> None:
        self._set_about_active("author")
        self._set_detail_html(
            "作者信息",
            "当前项目维护者与项目主页",
            """
            <p><span class='badge'>维护者</span> <b>ユイ可愛ね / zeli624233</b></p>
            <p><span class='badge'>GitHub</span> <a href='https://github.com/zeli624233'>https://github.com/zeli624233</a></p>
            <p><span class='badge'>项目主页</span> <a href='https://github.com/zeli624233/Cx2bro'>https://github.com/zeli624233/Cx2bro</a></p>

            <h2>项目说明</h2>
            <p><b>Cx2bro</b> 是 <b>zeli624233 / ユイ可愛ね</b> 维护的开源工具，适用于 krkrz 引擎、CxdecV2 (hxv4) 加密方式的 Galgame 解包与资源名还原。</p>
            <p>底层实现中沿用了 / 改写了 <b>YeLikesss</b> 的 <b>KrkrExtractForCxdecV2</b> 部分 C++ 代码与实现思路，因此在鸣谢与许可证页保留完整来源说明。</p>

            <h2>贡献者</h2>
            <ul>
                <li><b>YeLikesss</b> — CxdecV2 基础解包与动态哈希技术的奠基性工作（2023）</li>
                <li><b>Kinotern</b> — 优化密钥提取模块和批量解包界面（2024）</li>
                <li><b>zeli624233 / ユイ可愛ね</b> — 增强版本发布并最终重构定名为 Cx2bro（2026）</li>
            </ul>

            <h2>特别感谢</h2>
            <ul>
                <li><b>YuriSizuku</b> — 分享 CxdecV2 加密原理和哈希算法的技术文章</li>
                <li><b>GPT、Deepseek</b> — 在加密逆向思路和压缩算法优化方面提供的建议</li>
            </ul>

            <h2>当前项目主要改进方向</h2>
            <ul>
                <li>✅ PySide6 图形化工作台重构</li>
                <li>✅ 三大流程整合（静态 Hash / 动态提取 / 还原）</li>
                <li>✅ 扩展集制作、测试、安装与复用</li>
                <li>✅ 动态 Hash 收集器 C++ 原生窗口（实时 WM_COPYDATA 通信）</li>
                <li>✅ 运行时密钥提取（Hx/Cx/Verify 三探针 Detours Hook）</li>
                <li>✅ 批量 XP3 提取（Worker-Watchdog 架构）</li>
                <li>✅ 扩展集在线更新（GitHub 仓库索引）</li>
            </ul>

            <div class='box'>
                <span class='important'>使用说明：</span>本软件仅供学习、研究和个人备份使用。请勿用于传播、倒卖、侵犯游戏厂商或原作者权益的用途。
            </div>
            """,
        )

    def show_credits(self) -> None:
        self._set_about_active("credits")
        self._set_detail_html(
            "鸣谢 / 许可证",
            "原项目、参考资料与开源许可证说明",
            """
            <h2>1. C++ 底层代码来源 / 参考项目</h2>
            <p><b>KrkrExtractForCxdecV2</b>（CxdecV2 基础解包与动态哈希奠基性工作）</p>
            <p>作者：<b>YeLikesss / YeLike</b>（2023）</p>
            <p>仓库：<a href='https://github.com/YeLikesss/KrkrExtractForCxdecV2'>https://github.com/YeLikesss/KrkrExtractForCxdecV2</a></p>
            <div class='box'>
                <p><b>仓库关系说明：</b>当前 GitHub 仓库不是该项目的 Fork 仓库。</p>
                <p>但本项目的 CxdecV2 / krkrz_hxv4 底层提取、动态模块、Hash 处理等核心实现，沿用了 / 改写了该项目的部分 C++ 代码与实现思路，因此按代码来源保留原作者署名和许可证说明。</p>
            </div>

            <h2>2. 密钥提取模块改进</h2>
            <p><b>KrkrExtractForCxdecV3.3Extra_Plus</b></p>
            <p>作者：<b>Kinotern</b>（2024）</p>
            <p>优化了密钥提取模块和批量解包界面。</p>

            <h2>3. 逆向分析参考</h2>
            <p><b>KUNGal 论坛文章：</b>《Galgame汉化中的逆向（八）_哈希算法分析_以 krkrz_hxv4 为例》（作者：YuriSizuku）</p>
            <p>链接：<a href='https://www.kungal.com/topic/3155'>https://www.kungal.com/topic/3155</a></p>

            <h2>4. 当前项目维护</h2>
            <p>维护者：<b>zeli624233 / ユイ可愛ね</b></p>
            <p>项目主页：<a href='https://github.com/zeli624233/Cx2bro'>https://github.com/zeli624233/Cx2bro</a></p>

            <h2>5. 许可证</h2>
            <div class='box'>
                <p><span class='important'>本项目按 GNU AGPL-3.0 发布。</span></p>
                <p>代码来源决定了许可证。底层的 C++ 部分拿了 <b>YeLikesss</b> 和 <b>Kinotern</b> 的开源项目，它们都是 AGPL-3.0，我这里也跟着保持一致。</p>
            </div>
            <ul>
                <li>保留 YeLikesss / YeLike 原作者的署名、项目链接和许可证声明。</li>
                <li>保留我（zeli624233 / ユイ可愛ね）的二次开发和改动说明。</li>
                <li>你要改的话，也得用 AGPL-3.0 开源。</li>
                <li>发 exe 的时候把源码一起发，或者告诉别人去哪拿源码。</li>
                <li>别删第三方代码来源、贡献者和许可证信息。</li>
            </ul>

            <h2>免责声明</h2>
            <p class='soft'>本工具仅用于学习研究、资源备份和技术分析。请勿用于侵犯版权、传播商业游戏资源、倒卖软件或其他违法用途。因使用本工具造成的任何后果，由使用者自行承担。</p>
            """,
        )

class BatchExtractorDialog(QDialog):
    """动态 XP3 批量提取窗口。

    这一版先完成工作台集成、队列管理和路径绑定，实际批量执行等待核心
    CLI 增加专用 batch-extract 模式后再接入。
    """

    def __init__(self, parent: "WorkbenchWindow", mode: int, output_dir: Path) -> None:
        super().__init__(parent)
        self.workbench = parent
        self.mode = mode
        self.output_dir = output_dir
        self.queue_files: list[Path] = []

        self.setWindowTitle("Cx2bro XP3 批量提取")
        if parent.paths.app_icon.exists():
            self.setWindowIcon(QIcon(str(parent.paths.app_icon)))
        self.resize(860, 620)
        self.setMinimumSize(820, 560)
        self.setAcceptDrops(True)

        self.output_edit = QLineEdit(str(output_dir))
        self.output_edit.setReadOnly(True)
        self.thread_spin = QSpinBox()
        self.thread_spin.setRange(1, 8)
        self.thread_spin.setValue(1)
        self.detail_checkbox = QCheckBox("完成弹窗")
        self.detail_checkbox.setChecked(True)
        self.sound_checkbox = QCheckBox("完成提示音")
        self.sound_checkbox.setChecked(True)
        self.progress_bar = QProgressBar()
        self.progress_bar.setRange(0, 100)
        self.progress_bar.setValue(0)
        self.summary_label = QLabel("总任务 0 | 运行中 0 | 排队 0 | 完成 0 | 失败 0")

        self.table = QTableWidget(0, 4)
        self.table.setHorizontalHeaderLabels(["文件", "状态", "进度", "详细信息"])
        self.table.setEditTriggers(QAbstractItemView.NoEditTriggers)
        self.table.setSelectionBehavior(QAbstractItemView.SelectRows)
        self.table.setSelectionMode(QAbstractItemView.SingleSelection)
        self.table.setAlternatingRowColors(False)
        self.table.setAcceptDrops(False)
        self.table.verticalHeader().setVisible(False)
        header = self.table.horizontalHeader()
        header.setSectionResizeMode(0, QHeaderView.Stretch)
        header.setSectionResizeMode(1, QHeaderView.ResizeToContents)
        header.setSectionResizeMode(2, QHeaderView.ResizeToContents)
        header.setSectionResizeMode(3, QHeaderView.Stretch)

        self._build_ui()

    def _build_ui(self) -> None:
        layout = QVBoxLayout(self)
        layout.setContentsMargins(10, 10, 10, 10)
        layout.setSpacing(8)

        tip = QLabel("拖入 XP3 文件，或点击“添加 XP3 / 添加目录”加入队列。")
        layout.addWidget(tip)

        top_row = QHBoxLayout()
        top_row.addStretch(1)
        add_files = QPushButton("添加 XP3")
        add_files.clicked.connect(self.add_files)
        add_dir = QPushButton("添加目录")
        add_dir.clicked.connect(self.add_directory)
        clear_done = QPushButton("清空完成")
        clear_done.clicked.connect(self.clear_done_rows)
        top_row.addWidget(add_files)
        top_row.addWidget(add_dir)
        top_row.addWidget(clear_done)
        layout.addLayout(top_row)

        output_group = QGroupBox("输出目录")
        output_layout = QHBoxLayout(output_group)
        output_layout.addWidget(self.output_edit, 1)
        browse_button = QPushButton("浏览")
        browse_button.clicked.connect(lambda: self.workbench.open_path(self.output_dir))
        output_layout.addWidget(browse_button)
        layout.addWidget(output_group)

        options_row = QHBoxLayout()
        options_row.addWidget(self.detail_checkbox)
        options_row.addWidget(self.sound_checkbox)
        options_row.addSpacing(16)
        options_row.addWidget(QLabel("工作线程"))
        options_row.addWidget(self.thread_spin)
        options_row.addStretch(1)
        layout.addLayout(options_row)

        layout.addWidget(self.progress_bar)
        layout.addWidget(self.summary_label)
        layout.addWidget(self.table, 1)

        bottom_row = QHBoxLayout()
        start_button = QPushButton("开始")
        start_button.clicked.connect(self.start_batch_extract)
        remove_button = QPushButton("移除选中")
        remove_button.clicked.connect(self.remove_selected_row)
        open_output_button = QPushButton("打开输出目录")
        open_output_button.clicked.connect(lambda: self.workbench.open_path(self.output_dir))
        close_button = QPushButton("关闭")
        close_button.clicked.connect(self.close)
        bottom_row.addWidget(start_button)
        bottom_row.addWidget(remove_button)
        bottom_row.addStretch(1)
        bottom_row.addWidget(open_output_button)
        bottom_row.addWidget(close_button)
        layout.addLayout(bottom_row)

    def dragEnterEvent(self, event) -> None:  # type: ignore[override]
        if event.mimeData().hasUrls():
            event.acceptProposedAction()
        else:
            event.ignore()

    def dropEvent(self, event) -> None:  # type: ignore[override]
        urls = event.mimeData().urls()
        paths = [Path(url.toLocalFile()) for url in urls if url.isLocalFile()]
        self.add_paths(paths)
        event.acceptProposedAction()

    def add_files(self) -> None:
        files, _ = QFileDialog.getOpenFileNames(self, "选择 XP3 文件", "", "XP3 files (*.xp3);;All files (*.*)")
        self.add_paths([Path(path) for path in files])

    def add_directory(self) -> None:
        directory = QFileDialog.getExistingDirectory(self, "选择包含 XP3 的目录", "")
        if not directory:
            return
        paths = sorted(Path(directory).glob("*.xp3"))
        self.add_paths(paths)

    def add_paths(self, paths: list[Path]) -> None:
        added = 0
        for path in paths:
            if path.is_dir():
                for child in sorted(path.glob("*.xp3")):
                    added += self._add_queue_file(child)
            else:
                added += self._add_queue_file(path)
        if added:
            self.workbench.log(f"Batch Extractor: 已加入 {added} 个 XP3 任务到 User\\{self.mode} 队列。")
        self.update_summary()

    def _add_queue_file(self, path: Path) -> int:
        if path.suffix.lower() != ".xp3":
            return 0
        resolved = path.resolve()
        if resolved in self.queue_files:
            return 0
        self.queue_files.append(resolved)
        row = self.table.rowCount()
        self.table.insertRow(row)
        self.table.setItem(row, 0, QTableWidgetItem(resolved.name))
        self.table.setItem(row, 1, QTableWidgetItem("排队"))
        self.table.setItem(row, 2, QTableWidgetItem("0%"))
        self.table.setItem(row, 3, QTableWidgetItem(str(resolved)))
        return 1

    def remove_selected_row(self) -> None:
        row = self.table.currentRow()
        if row < 0:
            return
        path_item = self.table.item(row, 3)
        if path_item:
            target = Path(path_item.text())
            self.queue_files = [path for path in self.queue_files if path != target]
        self.table.removeRow(row)
        self.update_summary()

    def clear_done_rows(self) -> None:
        row = self.table.rowCount() - 1
        while row >= 0:
            status_item = self.table.item(row, 1)
            if status_item and status_item.text() in {"完成", "失败"}:
                path_item = self.table.item(row, 3)
                if path_item:
                    target = Path(path_item.text())
                    self.queue_files = [path for path in self.queue_files if path != target]
                self.table.removeRow(row)
            row -= 1
        self.update_summary()

    def update_summary(self) -> None:
        total = self.table.rowCount()
        queued = 0
        running = 0
        done = 0
        failed = 0
        for row in range(total):
            status_item = self.table.item(row, 1)
            status = status_item.text() if status_item else ""
            if status == "排队":
                queued += 1
            elif status == "运行中":
                running += 1
            elif status == "完成":
                done += 1
            elif status == "失败":
                failed += 1
        self.summary_label.setText(f"总任务 {total} | 运行中 {running} | 排队 {queued} | 完成 {done} | 失败 {failed}")
        percent = int(done / total * 100) if total else 0
        self.progress_bar.setValue(percent)

    def start_batch_extract(self) -> None:
        if self.table.rowCount() == 0:
            QMessageBox.information(self, APP_TITLE, "请先加入至少一个 XP3 文件。")
            return

        self.output_dir.mkdir(parents=True, exist_ok=True)
        task_file = self.write_task_file()
        for row in range(self.table.rowCount()):
            self.table.setItem(row, 1, QTableWidgetItem("运行中"))
            self.table.setItem(row, 2, QTableWidgetItem("等待原生窗口"))
            self.table.setItem(row, 3, QTableWidgetItem(str(self.queue_files[row])))
        self.update_summary()

        self.workbench.last_workspace = self.output_dir.parent
        self.workbench.log(
            f"Batch Extractor: User\\{self.mode} 准备启动批量解包，"
            f"输出目录 {self.output_dir}，任务 {self.table.rowCount()} 个，任务文件 {task_file}"
        )
        code, output = self.workbench.runner.run([
            "--mode", "batch-extract-xp3",
            "--game", self.workbench.game_path.text(),
            "--output-root", str(self.output_dir.parent),
            "--package-list", str(task_file),
            "--workers", str(self.thread_spin.value()),
            "--notify-popup", "1" if self.detail_checkbox.isChecked() else "0",
            "--notify-sound", "1" if self.sound_checkbox.isChecked() else "0",
        ])

        if code != 0:
            for row in range(self.table.rowCount()):
                self.table.setItem(row, 1, QTableWidgetItem("失败"))
                self.table.setItem(row, 2, QTableWidgetItem("启动失败"))
            self.update_summary()
            self.workbench.show_cli_summary(code, output)
            QMessageBox.warning(self, APP_TITLE, "启动批量 XP3 提取失败，请查看右侧信息和详细日志。")
            return

        self.workbench.show_cli_summary(code, output)
        QMessageBox.information(
            self,
            APP_TITLE,
            "已启动原生批量 XP3 提取窗口。\n\n"
            "后续进度会在注入后的原生解包窗口里显示。",
        )
        self.accept()

    def write_task_file(self) -> Path:
        temp_root = Path(tempfile.gettempdir()) / "cxdec_batch_tasks"
        temp_root.mkdir(parents=True, exist_ok=True)
        handle = tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            newline="\n",
            suffix=".txt",
            prefix=f"user{self.mode}_",
            dir=temp_root,
            delete=False,
        )
        with handle:
            for path in self.queue_files:
                handle.write(str(path))
                handle.write("\n")
        return Path(handle.name)


@dataclass
class ExtensionEntry:
    """扩展集里的一条游戏记录。"""

    brand: str
    game: str
    path: Path
    hash_seed: str = ""
    summary: str = ""
    contributor: str = ""
    version: str = ""
    date: str = ""
    has_rules: bool = False
    has_static_input: bool = False
    has_static_output: bool = False


class WorkbenchPaths:
    """集中管理 GUI 路径，避免路径拼接散落在界面代码里。"""

    def __init__(self, app_dir: Path) -> None:
        self.app_dir = app_dir
        self.extensions_dir = app_dir / "Extensions"
        self.logs_dir = app_dir / "Logs"
        self.cache_dir = app_dir / "Cache"
        # frozen 模式下资源文件打包在 _runtime/ 里（通过 --add-data）
        if getattr(sys, "frozen", False):
            self.app_icon = Path(sys._MEIPASS) / "app_icon.ico"
            self.about_avatar = Path(sys._MEIPASS) / "about_avatar.jpg"
        else:
            self.app_icon = app_dir / "app_icon.ico"
            self.about_avatar = app_dir / "about_avatar.jpg"
        self.core_cli = app_dir / "core" / "CxdecCoreCLI.exe"

    @staticmethod
    def user_workspace(game_dir: Path, mode: int) -> Path:
        return game_dir / "User" / str(mode)

    @staticmethod
    def publisher_workspace(game_dir: Path) -> Path:
        return game_dir / "Publisher"


class ExtensionCatalog:
    """扫描 Extensions\\会社\\游戏。

    扩展集默认只发布 manifest.int / rules.int。
    StaticHash_Output 属于运行时生成物，不强制打包在扩展集里。
    """

    def __init__(self, root: Path) -> None:
        self.root = root
        self.entries: list[ExtensionEntry] = []

    def load(self) -> list[ExtensionEntry]:
        self.entries.clear()
        self.root.mkdir(parents=True, exist_ok=True)

        for brand_dir in sorted(p for p in self.root.iterdir() if p.is_dir()):
            for game_dir in sorted(p for p in brand_dir.iterdir() if p.is_dir()):
                if self.is_backup_directory(game_dir):
                    continue
                self.entries.append(self._load_entry(brand_dir.name, game_dir))
        return self.entries

    @staticmethod
    def is_backup_directory(path: Path) -> bool:
        name = path.name
        if ".bak" not in name:
            return False
        suffix = name.rsplit(".bak", 1)[-1]
        return suffix.isdigit()

    def brands(self) -> list[str]:
        return sorted({entry.brand for entry in self.entries})

    def games_for_brand(self, brand: str) -> list[ExtensionEntry]:
        return [entry for entry in self.entries if entry.brand == brand]

    def _load_entry(self, brand: str, game_dir: Path) -> ExtensionEntry:
        rules_path = game_dir / "rules.int"
        manifest_path = game_dir / "manifest.int"
        entry = ExtensionEntry(
            brand=brand,
            game=game_dir.name,
            path=game_dir,
            has_rules=rules_path.exists(),
            has_static_input=False,
            has_static_output=(game_dir / "StaticHash_Output").exists(),
        )

        manifest_set_game = False
        manifest_set_brand = False
        def apply_meta(meta: dict[str, str], prefer_game_display: bool = False) -> None:
            nonlocal manifest_set_game, manifest_set_brand
            raw_brand = meta.get("brand", "")
            if raw_brand and raw_brand.lower() != "unknown" and not manifest_set_brand:
                entry.brand = raw_brand
                manifest_set_brand = True
            # 游戏名优先级：manifest.int 的 Game/GameDisplayName 优先于 rules.int 的 GameId
            game_name = meta.get("gamedisplayname") or meta.get("game") or meta.get("gameid")
            if game_name and not manifest_set_game:
                entry.game = game_name
                if meta.get("game") or meta.get("gamedisplayname"):
                    manifest_set_game = True
            if meta.get("hashseed"):
                entry.hash_seed = meta["hashseed"]
            if meta.get("summary"):
                entry.summary = meta["summary"]
            if meta.get("contributor") or meta.get("author"):
                entry.contributor = meta.get("contributor") or meta.get("author")
            if meta.get("version"):
                entry.version = meta["version"]
            raw_date = meta.get("date") or meta.get("builddate")
            if raw_date:
                entry.date = normalize_metadata_date(raw_date)

        if manifest_path.exists():
            apply_meta(self.read_int_meta(manifest_path))

        if rules_path.exists():
            apply_meta(self.read_int_meta(rules_path), prefer_game_display=True)

        if not entry.summary:
            entry.summary = "已提供 int 规则文件。" if entry.has_rules else "缺少 rules.int。"
        return entry

    @staticmethod
    def read_int_meta(path: Path) -> dict[str, str]:
        return read_int_sections_best_effort(path).get("meta", {})


def read_int_sections_best_effort(path: Path) -> dict[str, dict[str, str]]:
    """Read sections from text .int files and binary CXRI2/CXRI3 rules.int files.

    CXRI rules files start with a binary header before the textual [Meta]/[Pattern]
    blocks, so normal text reading misses the first section.  This helper honors
    the header sizes first and falls back to text parsing for manifest.int.
    """
    sections: dict[str, dict[str, str]] = {}
    try:
        data = path.read_bytes()
    except OSError:
        return sections

    text = ""
    try:
        if data.startswith(b"CXRI3\0") and len(data) >= 30:
            meta_size = int.from_bytes(data[6:10], "little", signed=False)
            pattern_size = int.from_bytes(data[10:14], "little", signed=False)
            start = 30
            end = start + meta_size + pattern_size
            if end <= len(data):
                text = data[start:end].decode("utf-8", errors="replace")
        elif data.startswith(b"CXRI2\0") and len(data) >= 22:
            meta_size = int.from_bytes(data[6:10], "little", signed=False)
            pattern_size = int.from_bytes(data[10:14], "little", signed=False)
            start = 22
            end = start + meta_size + pattern_size
            if end <= len(data):
                text = data[start:end].decode("utf-8", errors="replace")
        if not text:
            text = data.decode("utf-8-sig", errors="replace")
    except Exception:
        text = data.decode("utf-8-sig", errors="replace")

    current = ""
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith(";") or line.startswith("#"):
            continue
        if line.startswith("[") and "]" in line:
            # Allow trailing garbage after the section token, but keep the name strict.
            section_name = line[1:line.find("]")].strip().lower()
            current = section_name
            sections.setdefault(current, {})
            continue
        if current and "=" in line:
            key, value = line.split("=", 1)
            sections.setdefault(current, {})[key.strip().lower()] = value.strip()
    return sections


class CoreRunner:
    """Python GUI 到 C++ 核心 CLI 的边界。

    Python 只负责编排和显示；底层注入、解包、Hash 和还原交给 C++。
    """

    def __init__(self, paths: WorkbenchPaths, log) -> None:
        self.paths = paths
        self.log = log

    def run(self, args: list[str]) -> tuple[int, str]:
        if not self.paths.core_cli.exists():
            message = f"核心 CLI 未接入，跳过执行：{' '.join(args)}"
            self.log(message)
            return 0, message

        command = [str(self.paths.core_cli), *args]
        self.log("执行：" + " ".join(command))
        completed = subprocess.run(command, text=True, capture_output=True, encoding="utf-8", errors="replace", creationflags=subprocess.CREATE_NO_WINDOW)
        output_parts: list[str] = []
        if completed.stdout:
            output_parts.append(completed.stdout.rstrip())
            self.log(completed.stdout.rstrip())
        if completed.stderr:
            output_parts.append(completed.stderr.rstrip())
            self.log(completed.stderr.rstrip())
        return completed.returncode, "\n".join(part for part in output_parts if part)


class WorkbenchWindow(QMainWindow):
    """紧凑型工具界面。

    顶部是一级功能按钮，左侧是当前页操作区，右侧上半显示当前信息，
    右侧下半显示日志。整体尺寸按实际工作流控制，不做空旷大面板。
    """

    def __init__(self, paths: WorkbenchPaths) -> None:
        super().__init__()
        self.paths = paths
        self.catalog = ExtensionCatalog(paths.extensions_dir)
        self.runner = CoreRunner(paths, self.log)
        self.current_brand = ""

        self.setWindowTitle(f"{APP_TITLE} {app_version_label()}")
        if self.paths.app_icon.exists():
            self.setWindowIcon(QIcon(str(self.paths.app_icon)))
        self.resize(1080, 660)
        self.setMinimumSize(1080, 660)

        self.game_path = QLineEdit()
        self.user_game_path = QLineEdit()
        self._syncing_game_path = False
        self._last_game_path_context = ""
        self.game_path.textChanged.connect(self._sync_game_path_from_publisher)
        self.user_game_path.textChanged.connect(self._sync_game_path_from_user)
        self.publisher_brand = QLineEdit()
        self.publisher_game_name = QLineEdit()
        self.publisher_author = QLineEdit()
        self.publisher_version = QLineEdit("1.0")
        self.publisher_date = QLineEdit(today_text())
        self.publisher_summary = QLineEdit()
        self.info = QTextBrowser()
        self.log_box = QPlainTextEdit()
        self.user_flow_action_stack = QStackedWidget()
        # 左侧二级页面只保留本流程的可执行步骤；完整说明统一放到右侧“当前信息”，避免重复。
        self.user_flow_buttons: dict[int, QPushButton] = {}
        self.brand_list = QListWidget()
        self.brand_list.setObjectName("brandList")
        self.brand_list.setFocusPolicy(Qt.NoFocus)
        self.game_list = QListWidget()
        self.game_list.setObjectName("gameList")
        self.game_list.setFocusPolicy(Qt.NoFocus)
        self.user_brand_list = QListWidget()
        self.user_brand_list.setObjectName("brandList")
        self.user_brand_list.setFocusPolicy(Qt.NoFocus)
        self.user_game_list = QListWidget()
        self.user_game_list.setObjectName("gameList")
        self.user_game_list.setFocusPolicy(Qt.NoFocus)
        self.last_workspace: Path | None = None
        self.current_user_mode = 1
        self.last_restore_report: Path | None = None
        self.last_restored_dir: Path | None = None
        self.last_publisher_draft: Path | None = None
        self.last_publisher_report: Path | None = None
        self.last_publisher_source_workspace: Path | None = None
        self.custom_publisher_source_workspace: Path | None = None
        self.last_publisher_test_error = ""
        self.publisher_metadata_confirmed = False
        self.last_imported_extension: Path | None = None
        self.active_dynamic_session: dict[str, str | int] | None = None
        self.restore_processes: list[QProcess] = []
        self.publisher_test_process: QProcess | None = None
        self.publisher_test_poller: QTimer | None = None
        self.publisher_test_result_written = False
        self._skip_auto_refresh = False

        self._build_ui()
        self._apply_style()
        self._ensure_sample_extension()
        self.reload_extensions()
        self.log_environment_check_summary()

        # 自动刷新：每 3 秒更新首页流程概览、制作扩展集和扩展集信息
        self.auto_refresh_timer = QTimer(self)
        self.auto_refresh_timer.timeout.connect(self._auto_refresh_overview)
        self.auto_refresh_timer.start(3000)

        # C++ 窗口监控：检测 C++ 子窗口关闭后自动弹回前台（不受 Windows 前台锁限制）
        self._cpp_windows_were_active = False
        self._activate_timer = QTimer(self)
        self._activate_timer.timeout.connect(self._poll_activate_event)
        self._activate_timer.start(250)

    def _auto_refresh_overview(self) -> None:
        """每 3 秒自动刷新右侧面板（仅刷新当前可见的标签页）。"""
        current_tab = getattr(self, "_last_main_tab_index", 0)
        if current_tab == 0:  # 首页
            if self.current_user_mode == 1:
                self.update_user_flow_detail()
            elif self.current_user_mode == 2:
                self.update_user_extension_interaction()
            elif self.current_user_mode == 3:
                self.show_user_overview()
        elif current_tab == 1:  # 制作扩展集
            self.show_publisher_status()
        elif current_tab == 2:  # 扩展集管理
            if self._skip_auto_refresh:
                return  # 用户手动操作了按钮，不要覆盖右侧面板
            entry = self.current_extension_entry()
            if entry:
                self.show_extension_detail(entry)
            elif self.current_brand:
                self.show_extension_brand_info(self.current_brand, self.catalog.games_for_brand(self.current_brand))
            else:
                self.show_extension_default_info()

    def _poll_activate_event(self) -> None:
        """每 500ms 检查已知 C++ 子窗口是否还存在。检测到关闭则弹回前台。
        
        判断标准：枚举所有顶层窗口，若标题包含 Cx2bro + 特定关键词之一
        （"批量提取"、"hash模块"、"资源名还原"、"XP3 批量"），则记为 C++ 窗口活跃。
        从活跃→不活跃 时触发前台激活。
        """
        try:
            user32 = ctypes.windll.user32
            kernel32 = ctypes.windll.kernel32

            # 已知 C++ 窗口标题关键词（任一命中即算活跃）
            keywords = ("批量提取", "hash模块", "资源名还原", "XP3 批量")

            hwnd_list = []

            # 枚举顶层窗口找 C++ 子窗口
            def enum_proc(hwnd, _lParam):
                length = user32.GetWindowTextLengthW(hwnd) + 1
                buf = ctypes.create_unicode_buffer(length)
                user32.GetWindowTextW(hwnd, buf, length)
                title = buf.value
                if any(kw in title for kw in keywords):
                    hwnd_list.append(hwnd)
                return True

            EnumWindows = user32.EnumWindows
            EnumWindows.argtypes = [ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p), ctypes.c_void_p]
            EnumWindows.restype = ctypes.c_bool
            cb = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)(enum_proc)
            EnumWindows(cb, 0)

            cpp_active = len(hwnd_list) > 0

            # 保存状态并检测从活跃→不活跃的跳变
            if not hasattr(self, '_cpp_windows_were_active'):
                self._cpp_windows_were_active = False

            if self._cpp_windows_were_active and not cpp_active:
                # C++ 窗口刚消失了 → 弹回前台
                self.showNormal()
                self.raise_()
                hwnd = int(self.winId())
                # 多路激活
                try:
                    fn = user32.SwitchToThisWindow
                    fn.argtypes = [ctypes.c_void_p, ctypes.c_bool]
                    fn.restype = None
                    fn(hwnd, True)
                except Exception:
                    pass
                try:
                    user32.SetWindowPos(hwnd, 0, 0, 0, 0, 0, 0x0002 | 0x0001 | 0x0020)
                except Exception:
                    pass
                try:
                    user32.BringWindowToTop(hwnd)
                    user32.SetForegroundWindow(hwnd)
                except Exception:
                    pass
            self._cpp_windows_were_active = cpp_active
        except Exception:
            pass

    def _build_ui(self) -> None:
        root = QWidget()
        self.setCentralWidget(root)
        outer = QVBoxLayout(root)
        outer.setContentsMargins(12, 8, 12, 10)
        outer.setSpacing(8)

        header = QHBoxLayout()
        header.setSpacing(8)
        self.tabs = QTabBar()
        self.tabs.setExpanding(False)
        self._last_main_tab_index = 0
        self.tabs.addTab("首页")
        self.tabs.addTab("制作扩展集")
        self.tabs.addTab("扩展集")
        self.tabs.addTab("关于")
        self.tabs.currentChanged.connect(self._switch_page)
        header.addWidget(self.tabs)
        header.addStretch(1)
        outer.addLayout(header)

        center = QHBoxLayout()
        center.setSpacing(8)
        outer.addLayout(center, 1)

        self.stack = QStackedWidget()
        self.stack.setFixedWidth(470)
        center.addWidget(self.stack)

        right_column = QVBoxLayout()
        right_column.setSpacing(8)
        center.addLayout(right_column, 1)

        self.info_group = QGroupBox("流程概览")
        info_layout = QVBoxLayout(self.info_group)
        self.info.setReadOnly(True)
        self.info.setOpenExternalLinks(False)
        info_layout.addWidget(self.info)
        right_column.addWidget(self.info_group, 1)

        log_toolbar = QHBoxLayout()
        self.log_toggle_button = QPushButton("显示详细日志 ▼")
        self.log_toggle_button.clicked.connect(self.toggle_log_panel)
        self.copy_log_button = QPushButton("复制日志")
        self.copy_log_button.clicked.connect(self.copy_log_text)
        self.clear_log_button = QPushButton("清空日志")
        self.clear_log_button.clicked.connect(self.clear_log_text)
        self.copy_log_button.setVisible(False)
        self.clear_log_button.setVisible(False)
        log_toolbar.addWidget(self.log_toggle_button)
        log_toolbar.addWidget(self.copy_log_button)
        log_toolbar.addWidget(self.clear_log_button)
        log_toolbar.addStretch(1)
        right_column.addLayout(log_toolbar)

        self.log_group = QGroupBox("详细日志")
        self.log_group.setMaximumHeight(170)
        self.log_group.setMinimumHeight(120)
        log_layout = QVBoxLayout(self.log_group)
        log_layout.setContentsMargins(8, 8, 8, 8)
        log_layout.setSpacing(4)
        self.log_box.setReadOnly(True)
        self.log_box.setMaximumHeight(125)
        self.log_box.setMinimumHeight(90)
        log_layout.addWidget(self.log_box)
        self.log_group.setVisible(False)
        right_column.addWidget(self.log_group, 0)

        self.stack.addWidget(self._build_user_page())
        self.stack.addWidget(self._build_publisher_page())
        self.stack.addWidget(self._build_extension_page())
        self._switch_page(0)

    def _build_user_page(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(8)

        target_row_widget = QGroupBox("游戏的exe程序")
        target_row_widget.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        # 首页的目标游戏选择区与“制作扩展集”页使用同一套 GridLayout 参数，
        # 保证输入框宽度、选择按钮位置和切换页面时的视觉对齐一致。
        target_form = QGridLayout(target_row_widget)
        target_form.setHorizontalSpacing(6)
        target_form.setVerticalSpacing(6)
        target_form.addWidget(QLabel("目标游戏"), 0, 0)
        target_form.addWidget(self.user_game_path, 0, 1)
        target_form.addWidget(self._button("选择", self.select_game), 0, 2)
        target_form.setColumnStretch(1, 1)
        layout.addWidget(target_row_widget)

        group = QGroupBox("首页功能")
        form = QGridLayout(group)
        form.setHorizontalSpacing(6)
        form.setVerticalSpacing(6)

        self.user_flow_buttons.clear()
        for column, mode in enumerate((1, 2, 3)):
            button = QPushButton(USER_FLOW_DEFINITIONS[mode]["name"])
            button.setObjectName("flowButton")
            button.setCheckable(True)
            button.clicked.connect(lambda _checked=False, m=mode: self.select_user_flow(m))
            self.user_flow_buttons[mode] = button
            form.addWidget(button, 0, column)
        for column in range(3):
            form.setColumnStretch(column, 1)
        layout.addWidget(group)

        self.user_extension_area_stack = QStackedWidget()
        self.user_extension_area_stack.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)

        self.user_extension_group = QGroupBox("扩展集选择")
        ext_layout = QHBoxLayout(self.user_extension_group)
        brand_panel = QVBoxLayout()
        self.user_brand_label = QLabel("会社")
        brand_panel.addWidget(self.user_brand_label)
        brand_panel.addWidget(self.user_brand_list)
        game_panel = QVBoxLayout()
        self.user_game_label = QLabel("作品")
        game_panel.addWidget(self.user_game_label)
        game_panel.addWidget(self.user_game_list)
        ext_layout.addLayout(brand_panel)
        ext_layout.addLayout(game_panel)
        self.user_brand_list.currentTextChanged.connect(self.on_user_brand_selected)
        self.user_game_list.currentTextChanged.connect(self.on_user_game_selected)

        self.user_advanced_group = QGroupBox("高级操作")
        advanced_layout = QVBoxLayout(self.user_advanced_group)
        advanced_layout.setContentsMargins(16, 10, 16, 10)
        advanced_layout.addStretch(1)
        legacy_button = self._user_action_button(3, "CxdecExtractorLoader 原版功能", self.open_legacy_loader_dialog)
        self._fix_button_size(legacy_button, width=260, height=HOME_STEP_BUTTON_HEIGHT)
        advanced_layout.addWidget(legacy_button, 0, Qt.AlignmentFlag.AlignHCenter)
        advanced_layout.addStretch(1)

        self.user_extension_area_stack.addWidget(self.user_extension_group)
        self.user_extension_area_stack.addWidget(self.user_advanced_group)
        layout.addWidget(self.user_extension_area_stack, 1)

        self.user_flow_group = QWidget()
        flow_layout = QVBoxLayout(self.user_flow_group)
        flow_layout.setContentsMargins(0, 0, 0, 0)
        flow_layout.setSpacing(0)
        flow_layout.addWidget(self.user_flow_action_stack)
        self.user_flow_action_stack.addWidget(self._build_user_flow_page(1))
        self.user_flow_action_stack.addWidget(self._build_user_flow_page(2))
        self.user_flow_action_stack.addWidget(self._build_user_flow_page(3))
        layout.addWidget(self.user_flow_group, 2)

        self.set_active_user_flow(1, update_info=False, log_selection=False)
        return page

    def _build_user_flow_page(self, mode: int) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(0, 4, 0, 0)
        layout.setSpacing(HOME_FLOW_GROUP_SPACING)

        main_steps = QGroupBox("主流程")
        main_steps.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
        main_layout = QVBoxLayout(main_steps)
        main_layout.setContentsMargins(16, 10, 16, 10)
        main_layout.setSpacing(0)

        if mode == 1:
            step_buttons = [
                self._user_action_button(1, "步骤1：根据该作扩展集生成静态 Hash 表", lambda: self.run_user_flow_core(1, restore=False)),
                self._user_action_button(1, "步骤2：提取动态 XP3 资源", lambda: self.launch_dynamic("dynamic-extract")),
                self._user_action_button(1, "步骤3：用静态 Hash 还原资源名", lambda: self.open_restore_ui(1)),
            ]
        elif mode == 2:
            step_buttons = [
                self._user_action_button(2, "步骤1：根据会社集合生成静态 Hash 表", lambda: self.run_user_flow_core(2, restore=False)),
                self._user_action_button(2, "步骤2：提取动态 XP3 资源", lambda: self.launch_dynamic("dynamic-extract")),
                self._user_action_button(2, "步骤3：用集合 Hash 撞新作还原", lambda: self.open_restore_ui(2)),
            ]
        else:
            step_buttons = [
                self._user_action_button(3, "步骤1：提取动态 XP3 资源", lambda: self.launch_dynamic("dynamic-extract")),
                self._user_action_button(3, "步骤2：加载动态 Hash 收集模块", lambda: self.launch_dynamic("dynamic-stringhash")),
                self._user_action_button(3, "步骤3：用动态 Hash 还原资源名", lambda: self.open_restore_ui(3)),
            ]

        main_layout.addStretch(1)
        for index, button in enumerate(step_buttons):
            self._fix_button_size(button, width=400, height=HOME_STEP_BUTTON_HEIGHT)
            main_layout.addWidget(button, 0, Qt.AlignmentFlag.AlignHCenter)
            if index != len(step_buttons) - 1:
                main_layout.addSpacing(HOME_FLOW_ROW_SPACING)
        main_layout.addStretch(1)

        aux_group = QGroupBox("辅助操作")
        aux_group.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
        aux_layout = QVBoxLayout(aux_group)
        aux_layout.setContentsMargins(16, 10, 16, 10)
        aux_layout.setSpacing(0)

        aux_button_width = 170
        aux_gap = 12
        aux_full_width = aux_button_width * 2 + aux_gap

        def prepare_aux_button(button: QPushButton, *, full_width: bool = False) -> QPushButton:
            self._fix_button_size(button, width=aux_full_width if full_width else aux_button_width, height=HOME_AUX_BUTTON_HEIGHT)
            return button

        def add_aux_row(*buttons: QPushButton) -> None:
            row = QHBoxLayout()
            row.setContentsMargins(0, 0, 0, 0)
            row.setSpacing(aux_gap)
            row.addStretch(1)
            for button in buttons:
                row.addWidget(prepare_aux_button(button))
            row.addStretch(1)
            aux_layout.addLayout(row)

        status_button = self._user_action_button(mode, "检查状态", self.inspect_user_flow_status)
        status_row = QHBoxLayout()
        status_row.setContentsMargins(0, 0, 0, 0)
        status_row.addStretch(1)
        status_row.addWidget(prepare_aux_button(status_button, full_width=True))
        status_row.addStretch(1)
        aux_layout.addStretch(1)
        aux_layout.addLayout(status_row)
        aux_layout.addSpacing(HOME_AUX_ROW_SPACING)
        add_aux_row(
            self._user_action_button(mode, "打开工作区", self.open_current_workspace),
            self._user_action_button(mode, "清理目录", self.clean_current_workspace),
        )
        aux_layout.addSpacing(HOME_AUX_ROW_SPACING)
        add_aux_row(
            self._user_action_button(mode, "打开还原目录", self.open_last_restored_dir),
            self._user_action_button(mode, "查看还原报告", self.open_last_restore_report),
        )
        aux_layout.addStretch(1)

        layout.addWidget(main_steps, 1)
        layout.addWidget(aux_group, 1)
        return page

    def _build_publisher_page(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(8)

        group = QGroupBox("制作扩展集")
        form = QGridLayout(group)
        form.setHorizontalSpacing(6)
        form.setVerticalSpacing(6)
        form.addWidget(QLabel("目标游戏"), 0, 0)
        form.addWidget(self.game_path, 0, 1)
        form.addWidget(self._button("选择", self.select_game), 0, 2)
        form.addWidget(self._button("生成当前游戏扩展集", self.generate_minimal_extension), 1, 0, 1, 3)
        form.addWidget(self._button("编辑当前扩展集信息", self.edit_current_publisher_metadata), 2, 0, 1, 3)
        form.addWidget(self._button("测试当前扩展集", self.test_current_publisher_extension), 3, 0, 1, 3)
        form.setColumnStretch(1, 1)
        layout.addWidget(group)

        directory_group = QGroupBox("目录")
        directory_layout = QGridLayout(directory_group)
        directory_layout.setHorizontalSpacing(6)
        directory_layout.setVerticalSpacing(6)
        directory_layout.addWidget(self._button("打开当前扩展集目录", self.open_last_publisher_draft), 0, 0, 1, 3)
        directory_layout.addWidget(self._button("安装到扩展集库", self.import_extension_draft), 1, 0, 1, 3)
        directory_layout.addWidget(self._button("打开扩展集库目录", lambda: self.open_path(self.paths.extensions_dir)), 2, 0, 1, 3)
        for column in range(3):
            directory_layout.setColumnStretch(column, 1)
        layout.addWidget(directory_group)

        custom_group = QGroupBox("自定义")
        custom_layout = QGridLayout(custom_group)
        custom_layout.setHorizontalSpacing(6)
        custom_layout.setVerticalSpacing(6)
        custom_layout.addWidget(self._button("选择扩展集来源目录", self.select_publisher_extension_source_directory), 0, 0, 1, 3)
        custom_layout.addWidget(self._button("选择测试来源目录", self.select_publisher_source_workspace), 1, 0, 1, 3)
        for column in range(3):
            custom_layout.setColumnStretch(column, 1)
        layout.addWidget(custom_group)

        layout.addStretch(1)
        return page

    def _build_extension_page(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(8)

        group = QGroupBox("扩展集管理与浏览")
        group_layout = QVBoxLayout(group)

        toolbar = QGridLayout()
        toolbar.setHorizontalSpacing(6)
        toolbar.setVerticalSpacing(6)
        def wrap_with_skip(fn):
            def wrapper():
                self._skip_auto_refresh = True
                fn()
            return wrapper
        toolbar.addWidget(self._button("刷新列表", wrap_with_skip(self.reload_extensions)), 0, 0)
        toolbar.addWidget(self._button("打开扩展集目录", wrap_with_skip(lambda: self.open_path(self.paths.extensions_dir))), 0, 1)
        toolbar.addWidget(self._button("打开选中扩展集", wrap_with_skip(self.open_selected_extension)), 0, 2)
        toolbar.addWidget(self._button("在线更新", wrap_with_skip(self.check_online_updates)), 1, 0)
        toolbar.addWidget(self._button("运行环境自检", wrap_with_skip(self.show_environment_check)), 1, 1)
        toolbar.addWidget(self._button("扩展集留言", wrap_with_skip(self.show_extension_description)), 1, 2)
        for column in range(3):
            toolbar.setColumnStretch(column, 1)
        group_layout.addLayout(toolbar)

        lists = QHBoxLayout()
        brand_panel = QVBoxLayout()
        brand_panel.addWidget(QLabel("会社"))
        brand_panel.addWidget(self.brand_list)
        game_panel = QVBoxLayout()
        game_panel.addWidget(QLabel("作品"))
        game_panel.addWidget(self.game_list)
        lists.addLayout(brand_panel)
        lists.addLayout(game_panel)
        group_layout.addLayout(lists, 1)

        self.brand_list.currentTextChanged.connect(self.on_brand_selected)
        self.game_list.currentTextChanged.connect(self.on_game_selected)
        layout.addWidget(group, 1)
        return page

    def _button(self, text: str, slot) -> QPushButton:
        button = QPushButton(text)
        button.clicked.connect(slot)
        return button

    def _fix_button_width_to_text(self, button: QPushButton, *, extra: int = 44, min_width: int = 90, max_width: int = 420) -> QPushButton:
        """保留给少量需要按文字自适应的按钮使用。"""
        text_width = button.fontMetrics().horizontalAdvance(button.text())
        width = max(min_width, min(max_width, text_width + extra))
        return self._fix_button_size(button, width=width, height=max(30, button.minimumHeight()))

    def _fix_button_size(self, button: QPushButton, *, width: int, height: int = 30) -> QPushButton:
        button.setMinimumWidth(width)
        button.setMaximumWidth(width)
        button.setMinimumHeight(height)
        button.setMaximumHeight(height)
        button.setSizePolicy(QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Fixed)
        return button

    def _user_action_button(self, mode: int, text: str, slot, *, fit_width: bool = False) -> QPushButton:
        button = QPushButton(text)
        if text.startswith("步骤"):
            button.setObjectName("stepButton")
        button.setMinimumHeight(30)
        if fit_width:
            self._fix_button_width_to_text(button, extra=48, min_width=110, max_width=430)
        button.clicked.connect(lambda _checked=False, m=mode, action=slot: self.run_user_action(m, action))
        return button

    def _sync_game_path_from_publisher(self, text: str) -> None:
        if self._syncing_game_path:
            return
        self._syncing_game_path = True
        try:
            if self.user_game_path.text() != text:
                self.user_game_path.setText(text)
        finally:
            self._syncing_game_path = False
        self.on_game_path_context_changed(text)

    def _sync_game_path_from_user(self, text: str) -> None:
        if self._syncing_game_path:
            return
        self._syncing_game_path = True
        try:
            if self.game_path.text() != text:
                self.game_path.setText(text)
        finally:
            self._syncing_game_path = False
        self.on_game_path_context_changed(text)

    def on_game_path_context_changed(self, text: str) -> None:
        """Reset publisher state when the target game changes.

        The publisher test source must belong to the currently selected game.
        Keeping the previous game's User\1/2/3 workspace caused cross-game tests
        to pass against stale restore output and made the generated report misleading.
        """
        signature = text.strip()
        if signature == getattr(self, "_last_game_path_context", ""):
            return
        had_previous = bool(getattr(self, "_last_game_path_context", ""))
        self._last_game_path_context = signature
        if not had_previous:
            return
        self.last_publisher_source_workspace = None
        self.custom_publisher_source_workspace = None
        self.last_publisher_draft = None
        self.last_publisher_report = None
        self.last_publisher_test_error = ""
        self.publisher_metadata_confirmed = False
        self.publisher_test_result_written = False
        self.log("目标游戏已变更，已清空上一个游戏的扩展集草稿和测试来源，避免串用旧游戏目录。")

    def run_user_action(self, mode: int, action) -> None:
        self.set_active_user_flow(mode, update_info=False, log_selection=False)
        action()
        self.show_user_overview()

    def _switch_page(self, index: int) -> None:
        if index == 3:
            self.show_about_dialog()
            self.tabs.setCurrentIndex(getattr(self, "_last_main_tab_index", 0))
            return

        self._last_main_tab_index = index
        self.stack.setCurrentIndex(index)
        if index == 0:
            self.populate_user_extension_lists()
            self.update_user_flow_detail()
        elif index == 1:
            self.show_publisher_status()
        elif index == 2:
            entry = self.current_extension_entry()
            if entry:
                self.show_extension_detail(entry)
            elif self.current_brand:
                self.show_extension_brand_info(self.current_brand, self.catalog.games_for_brand(self.current_brand))
            else:
                self.show_extension_default_info()

    def _apply_style(self) -> None:
        self.setStyleSheet(
            """
            QWidget {
                font-family: "Microsoft YaHei UI", "Segoe UI";
                font-size: 12px;
                color: #111111;
            }
            QMainWindow, QDialog {
                background: #f0f0f0;
            }
            QLabel#appTitle {
                font-size: 13px;
                font-weight: 600;
                padding-left: 2px;
            }
            QTabBar::tab {
                background: #efefef;
                border: 1px solid #a8a8a8;
                padding: 5px 22px;
                margin-left: 4px;
            }
            QTabBar::tab:selected {
                background: #f8f8f8;
                border-bottom-color: #f8f8f8;
            }
            QGroupBox {
                background: #f0f0f0;
                border: 1px solid #d6d6d6;
                margin-top: 8px;
                padding: 8px;
            }
            QGroupBox::title {
                subcontrol-origin: margin;
                left: 10px;
                padding: 0 4px;
                font-weight: 600;
                background: #f0f0f0;
            }
            QMenu::item:hover {
                background: #0078D7;
                color: #FFFFFF;
            }
            QMenuBar::item:hover {
                background: #0078D7;
                color: #FFFFFF;
            }
            QListWidget::item:hover {
                background: #e0e0e0;
            }
            QGroupBox#aboutFrame {
                margin-top: 0px;
                padding: 8px;
            }
            QGroupBox#aboutFrame::title {
                height: 0px;
                padding: 0px;
            }
            QGroupBox#targetFrame {
                border: 1px solid #d6d6d6;
                margin-top: 8px;
                padding: 8px;
                background: #f0f0f0;
            }
            QGroupBox#targetFrame::title {
                subcontrol-origin: margin;
                left: 10px;
                padding: 0 4px;
                font-weight: 600;
                background: #f0f0f0;
            }
            QLineEdit, QPlainTextEdit, QTextBrowser, QListWidget {
                background: #ffffff;
                border: 1px solid #9d9d9d;
                padding: 3px;
                selection-background-color: #d7d7d7;
                selection-color: #111111;
            }
            QTableWidget {
                background: #ffffff;
                border: 1px solid #9d9d9d;
                gridline-color: #dcdcdc;
                selection-background-color: #d7d7d7;
                selection-color: #111111;
            }
            QHeaderView::section {
                background: #efefef;
                border: 1px solid #c6c6c6;
                padding: 4px 6px;
                font-weight: 600;
            }
            QProgressBar {
                background: #ffffff;
                border: 1px solid #9d9d9d;
                text-align: center;
                min-height: 18px;
            }
            QProgressBar::chunk {
                background: #4a89dc;
            }
            QSpinBox {
                background: #ffffff;
                border: 1px solid #9d9d9d;
                padding: 2px 4px;
                min-width: 44px;
            }
            QCheckBox {
                spacing: 6px;
            }
            QPushButton {
                background: #e9e9e9;
                border: 1px solid #a8a8a8;
                padding: 5px 10px;
                min-height: 18px;
            }
            QPushButton:hover {
                background: #f6f6f6;
                border-color: #8f8f8f;
            }
            QPushButton#flowButton:hover {
                background: #f6f6f6;
                border-color: #8f8f8f;
            }
            QPushButton:pressed {
                background: #dddddd;
            }
            QPushButton:checked {
                background: #e9e9e9;
                border: 2px solid #707070;
                font-weight: 600;
            }
            QPushButton#aboutOptionButton:checked {
                background: #e9e9e9;
                border: 2px solid #0078d7;
                font-weight: 600;
            }
            QPushButton#aboutOptionButton:checked:hover {
                background: #f0f0f0;
                border-color: #0078d7;
            }
            QPushButton#flowButton:checked {
                background: #e9e9e9;
                border: 2px solid #0078d7;
                font-weight: 600;
            }
            QPushButton#flowButton:checked:hover {
                background: #f0f0f0;
                border-color: #0078d7;
            }
            QPushButton#stepButton {
                text-align: left;
                padding-left: 16px;
            }
            QPushButton#legacyLoaderButton:focus {
                background: #f6f6f6;
                border: 2px solid #0078d7;
            }
            QPushButton#legacyLoaderButton:hover {
                background: #f6f6f6;
                border-color: #8f8f8f;
            }
            QListWidget::item {
                padding: 3px 4px;
            }
            QListWidget:disabled {
                background: #f7f7f7;
                color: #555555;
                border: 1px solid #b8b8b8;
            }
            QListWidget#brandList::item:selected {
                background: #cce5ff;
                color: #111111;
            }
            QListWidget#gameList::item:selected {
                background: #d4edda;
                color: #111111;
            }
            """
        )

    def update_user_extension_interaction(self) -> None:
        """根据首页主功能控制扩展集列表交互。

        功能 1 需要“会社 + 游戏”两个选择；功能 2 只使用会社集合，
        右侧游戏列表保留可见但禁用，避免用户误以为会按单个游戏扩展集执行。
        功能 3 不使用扩展集选择，整个区域由外层逻辑隐藏。
        """
        if not hasattr(self, "user_game_list"):
            return

        company_collection_mode = self.current_user_mode == 2
        self.user_game_list.setEnabled(not company_collection_mode)
        if hasattr(self, "user_game_label"):
            self.user_game_label.setText("作品（仅查看）" if company_collection_mode else "作品")
        tooltip = "该会社集合撞新作模式只使用左侧会社；右侧游戏列表仅供查看，不参与生成。" if company_collection_mode else ""
        self.user_game_list.setToolTip(tooltip)
        if hasattr(self, "user_game_label"):
            self.user_game_label.setToolTip(tooltip)

    def format_path(self, path: Path | None) -> str:
        return str(path) if path else "未选择"

    def current_game_path_text(self) -> str:
        value = self.game_path.text().strip()
        return value if value else "未选择"

    def user_workspace_path_without_prompt(self, mode: int | None = None) -> Path | None:
        value = self.game_path.text().strip()
        if not value:
            return None
        return WorkbenchPaths.user_workspace(Path(value).parent, self.current_user_mode if mode is None else mode)

    def format_timestamp(self, path: Path) -> str:
        try:
            return datetime.fromtimestamp(path.stat().st_mtime).strftime("%Y-%m-%d %H:%M")
        except OSError:
            return ""

    def count_direct_items(self, path: Path) -> int:
        if not path.exists():
            return 0
        if path.is_file():
            return 1
        try:
            return sum(1 for child in path.iterdir() if child.name not in (".", ".."))
        except OSError:
            return 0

    def status_text_for_dir(self, path: Path | None, label: str) -> str:
        if not path:
            return f"{label}：未选择目标游戏"
        if self.has_output_content(path):
            count = self.count_direct_items(path)
            timestamp = self.format_timestamp(path)
            suffix = f"，{count} 项" if count else ""
            if timestamp:
                suffix += f"，{timestamp}"
            return f"{label}：已生成{f'（{suffix.lstrip("，")}）' if suffix else ''}"
        if path.exists():
            timestamp = self.format_timestamp(path)
            return f"{label}：目录存在但为空{f'（{timestamp}）' if timestamp else ''}"
        return f"{label}：未生成"

    def parse_restore_report_values(self, report: Path) -> dict[str, str]:
        if not report.exists():
            return {}
        content = self.read_text_best_effort(report)
        values: dict[str, str] = {}
        key_map = {
            "总文件数": "TOTAL_FILES",
            "成功还原": "RESTORED_FILES",
            "最终还原": "RESTORED_FILES",  # 推理补充后的最终值，出现在"成功还原"之后，优先级覆盖
            "缺少目录 Hash": "MISSING_DIRECTORY_HASH",
            "缺少文件名 Hash": "MISSING_FILE_NAME_HASH",
            "复制失败": "COPY_FAILED",
            "TOTAL_FILES": "TOTAL_FILES",
            "RESTORED_FILES": "RESTORED_FILES",
            "MISSING_DIRECTORY_HASH": "MISSING_DIRECTORY_HASH",
            "MISSING_FILE_NAME_HASH": "MISSING_FILE_NAME_HASH",
            "COPY_FAILED": "COPY_FAILED",
        }
        for raw_line in content.splitlines():
            line = raw_line.strip()
            if ":" not in line:
                continue
            key, value = line.split(":", 1)
            mapped = key_map.get(key.strip())
            if mapped:
                raw = value.strip()
                # "最终还原"行格式为 "17316 / 18216"，只取前半部分数字
                if "/" in raw:
                    raw = raw.split("/", 1)[0].strip()
                values[mapped] = raw
        return values

    def parse_restore_package_details(self, report: Path) -> list[tuple[str, int, int, str]]:
        """Parse per-directory restore stats from RestoreReport.txt.

        Returns list of (package_name, restored, total, rate_str).
        """
        if not report.exists():
            return []
        results: list[tuple[str, int, int, str]] = []
        in_section = False
        for raw_line in self.read_text_best_effort(report).splitlines():
            line = raw_line.strip()
            if line == "--- 各目录还原情况 ---":
                in_section = True
                continue
            if not in_section or not line or line.startswith("---"):
                continue
            parts = line.split("│")
            if len(parts) >= 3:
                name = parts[0].strip()
                restored_total = parts[1].strip()
                rate = parts[2].strip()
                if "/" in restored_total:
                    try:
                        r = int(restored_total.split("/")[0].strip())
                        t = int(restored_total.split("/")[1].strip())
                        results.append((name, r, t, rate))
                    except (ValueError, IndexError):
                        pass
        return results

    def restore_summary_lines(self, workspace: Path | None) -> list[str]:
        if not workspace:
            return ["还原报告：未选择目标游戏"]
        report = workspace / "Restored_Extractor_Output" / "RestoreReport.txt"
        if not report.exists():
            return ["还原报告：未生成", f"位置：{report}"]
        values = self.parse_restore_report_values(report)
        counts = self.parse_restore_report_counts(report)
        total = int(values.get("TOTAL_FILES") or counts.get("total", 0))
        restored = int(values.get("RESTORED_FILES") or counts.get("restored", 0))
        rate = f"{restored / max(total, 1) * 100:.2f}%" if total else "0.00%"
        lines = [
            "还原报告：已生成",
            f"成功还原：{restored} / {total}",
            f"成功率：{rate}",
        ]
        if values.get("MISSING_DIRECTORY_HASH"):
            lines.append(f"缺少目录 Hash：{values['MISSING_DIRECTORY_HASH']}")
        if values.get("MISSING_FILE_NAME_HASH"):
            lines.append(f"缺少文件名 Hash：{values['MISSING_FILE_NAME_HASH']}")
        if values.get("COPY_FAILED"):
            lines.append(f"复制失败：{values['COPY_FAILED']}")
        timestamp = self.format_timestamp(report)
        if timestamp:
            lines.append(f"报告时间：{timestamp}")
        lines.append(f"报告位置：{report}")
        return lines

    def workflow_progress_lines(self, mode: int, workspace: Path | None) -> list[str]:
        if not workspace:
            return ["请先选择目标游戏。"]
        if mode in (1, 2):
            return [
                "1. " + self.status_text_for_dir(workspace / "StaticHash_Output", "静态 Hash 表"),
                "2. " + self.status_text_for_dir(workspace / "Extractor_Output", "动态 XP3 输出"),
                "3. " + self.status_text_for_dir(workspace / "Restored_Extractor_Output", "资源名还原"),
            ]
        return [
            "1. " + self.status_text_for_dir(workspace / "Extractor_Output", "动态 XP3 输出"),
            "2. " + self.status_text_for_dir(workspace / "StringHashDumper_Output", "动态 Hash 输出"),
            "3. " + self.status_text_for_dir(workspace / "Restored_Extractor_Output", "资源名还原"),
        ]

    def next_step_for_user_mode(self, mode: int, workspace: Path | None) -> str:
        if not workspace:
            return "← 先在左侧选择目标游戏路径。"
        static_output = workspace / "StaticHash_Output"
        extractor_output = workspace / "Extractor_Output"
        dynamic_output = workspace / "StringHashDumper_Output"
        restored_output = workspace / "Restored_Extractor_Output"
        if mode in (1, 2) and not self.has_output_content(static_output):
            return "步骤1：生成静态 Hash 表。"
        if not self.has_output_content(extractor_output):
            step = "步骤2" if mode in (1, 2) else "步骤1"
            return f"{step}：提取动态 XP3 资源。"
        if mode == 3 and not self.has_output_content(dynamic_output):
            return "步骤2：加载动态 Hash 收集模块并进游戏跑 Hash。"
        if not self.has_output_content(restored_output):
            return "步骤3：执行资源名还原。"
        return "流程已完成，查看还原目录或还原报告。"

    def selected_user_extension_cards(self) -> list[tuple[str, list[str] | str]]:
        mode = self.current_user_mode
        if mode == 3:
            return []

        brand = self.current_user_brand()
        if mode == 2:
            if not brand:
                return [("会社集合状态", ["未选择会社。", "请选择左侧会社；该模式会合并该会社下全部游戏扩展集。"])]
            entries = self.catalog.games_for_brand(brand)
            games = [entry.game for entry in entries]
            game_preview = "、".join(games[:8]) if games else "无"
            if len(games) > 8:
                game_preview += f" …… 共 {len(games)} 个"
            available = sum(1 for entry in entries if entry.has_rules or entry.has_static_input or entry.has_static_output)
            return [
                ("会社集合状态", [
                    f"会社：{brand}",
                    f"已搭载游戏扩展集：{len(entries)} 个",
                    f"可参与合并：{available} 个",
                    f"包含游戏：{game_preview}",
                    "当前模式只使用会社集合，右侧游戏列表仅供查看。",
                ])
            ]

        entry = self.current_user_extension()
        if not brand:
            return [("扩展集状态", ["未选择会社。", "请先在左侧选择会社，再选择该作游戏扩展集。"])]
        if not entry:
            return [("扩展集状态", [f"会社：{brand}", "未选择游戏扩展集。", "请选择右侧游戏，右边会显示扩展集内容。"])]
        ready = "可用" if entry.has_rules and entry.hash_seed else "需检查"
        validation = self.extension_validation_info(entry.path)
        return [
            ("扩展集状态", [
                f"会社：{entry.brand}",
                f"游戏：{entry.game}",
                f"版本：{entry.version or '未填写'}",
                f"日期：{entry.date or '未填写'}",
                f"贡献者：{entry.contributor or '未填写'}",
                f"成功率：{validation.get('success_rate') or '未测试'}",
                f"留言：{entry.summary or '未填写'}",
                f"HashSeed：{entry.hash_seed or '(未设置)'}",
                f"规则状态：{ready}",
                f"路径：{entry.path}",
            ]),
            ("扩展集内容", self.extension_file_count_lines(entry)),
        ]

    def home_flow_intro_lines(self, mode: int) -> list[str]:
        data = USER_FLOW_DEFINITIONS.get(mode, USER_FLOW_DEFINITIONS[1])
        help_title = f"功能 {mode}：{data['name']}"
        help_text = next(
            (detail for title, detail in HELP_ITEMS.get("首页", []) if title == help_title),
            data["target"],
        )
        return [help_title, help_text]

    def show_user_overview(self) -> None:
        mode = self.current_user_mode
        workspace = self.user_workspace_path_without_prompt(mode)
        data = USER_FLOW_DEFINITIONS.get(mode, USER_FLOW_DEFINITIONS[1])
        cards: list[tuple[str, list[str] | str]] = [
            ("功能介绍", self.home_flow_intro_lines(mode)),
            ("当前任务", [
                f"模式：{mode}. {data['name']}",
                f"目标游戏：{self.current_game_path_text()}",
                f"工作区：{self.format_path(workspace)}",
            ]),
        ]
        cards.extend(self.selected_user_extension_cards())
        cards.extend([
            ("流程进度", self.workflow_progress_lines(mode, workspace)),
            ("下一步", [self.next_step_for_user_mode(mode, workspace)]),
            ("还原结果", self.restore_summary_lines(workspace)),
        ])
        if workspace:
            output_lines = data["output"].replace("输出目录：", "").rstrip("。").split("、")
            cards.append(("关键目录", output_lines))
        self.set_info_cards("流程概览", cards)

    def show_publisher_status(self) -> None:
        value = self.game_path.text().strip()
        game_dir = Path(value).parent if value else None
        draft = self.current_publisher_draft_path(game_dir)
        installed = self.last_imported_extension
        manifest = draft / "manifest.int" if draft else None
        rules = draft / "rules.int" if draft else None
        package_mode = ""
        has_voice_pattern = False
        package_kb = self.publisher_package_size_kb(draft) if draft and draft.exists() else ""
        if manifest and manifest.exists():
            manifest_meta = ExtensionCatalog.read_int_meta(manifest)
            package_mode = manifest_meta.get("packagemode", "")
        rules_ok = False
        rules_issue = "未生成"
        if rules and rules.exists():
            rules_ok, rules_issue = self.rules_int_integrity(rules)
            rules_meta = self.read_rules_meta(rules)
            has_voice_pattern = bool(rules_meta.get("voicepattern", ""))
        validation_info = self.extension_validation_info(draft) if draft and draft.exists() else {}
        validation_passed = validation_info.get("passed", "")
        validation_restored = validation_info.get("restored_files", "")
        validation_total = validation_info.get("total_files", "")
        validation_error = validation_info.get("error", "")
        validation_package_kb = validation_info.get("package_size_kb", "")
        validation_basis = validation_info.get("pass_basis", "")
        metadata = self.read_draft_metadata(draft)
        detail_lines = self.extension_detail_lines_for_path(draft) if draft and draft.exists() else []
        status_lines: list[tuple[str, list[str] | str]] = [
            ("功能介绍", [
                "制作扩展集",
                "这里用于为当前目标游戏制作可复用的扩展集。先选择目标游戏，再生成当前游戏扩展集，编辑会社、游戏名、贡献者等信息；测试通过后安装到扩展集库，首页的该作扩展集提取和会社集合撞新作就可以使用它。",
            ]),
            ("测试结果（仅供参考）", [
                f"来源目录：{self.publisher_source_label(game_dir)}",
                f"扩展集来源：{self.format_path(draft) if draft else '未选择'}",
                f"测试状态：{'已测试' if validation_passed else '未测试'}",
                f"测试结果：{validation_restored or '-'} / {validation_total or '-'}",
                f"成功率：{self.restore_rate_text(validation_restored, validation_total)}",
                f"扩展集大小：{validation_package_kb or package_kb or '-'} KB",
                f"rules.int 状态：{'正常' if rules_ok else rules_issue}",
                f"测试问题：{self.last_publisher_test_error or validation_error or '无'}",
                f"VoicePattern：{'存在' if has_voice_pattern else '无'}",
            ]),
        ]
        self.set_info_cards(
            "制作状态",
            status_lines + [
                ("下一步建议", ["先生成当前游戏扩展集，确认并保存游戏名、会社、贡献者等信息；然后测试当前扩展集查看还原效果，满意后再安装到扩展集库。"]),
            ] + detail_lines + [
                ("安装位置", [self.format_path(installed) if installed else '尚未安装']),
            ],
        )

    def show_extension_default_info(self) -> None:
        self.set_info_cards(
            "扩展集详情",
            [
                ("功能介绍", ["扩展集是什么", "扩展集用于保存某个会社或某个游戏已经整理好的 Hash、文件名、目录名和规则信息。它的目标是提高 CxdecV2 资源名还原率，避免每次都完全依赖动态 Hash 收集。"]),
                ("扩展集管理与浏览", ["这里用于查看当前软件已搭载的扩展集。", "左侧选择会社，右侧选择游戏扩展集后，会自动显示扩展集内容、路径、结构和可用状态。"]),
                ("按钮说明", ["刷新列表：重新扫描 Extensions 目录。", "打开扩展集目录：打开全部扩展集所在目录。", "打开选中扩展集：打开当前选中的会社/游戏扩展集目录。", "在线更新：从 GitHub 仓库检查并下载新增扩展集。", "运行环境自检：检查核心程序、DLL、扩展集目录是否正常。", "扩展集留言：查看扩展集作用和目录结构说明。"]),
            ],
        )

    def show_extension_brand_info(self, brand: str, entries: list[ExtensionEntry]) -> None:
        if not brand:
            self.show_extension_default_info()
            return
        self.set_info_cards(
            "扩展集详情",
            [
                ("会社集合", [f"会社：{brand}", f"已搭载游戏扩展集数量：{len(entries)}", "可用于：首页功能2“该会社集合撞新作”。"]),
                ("下一步建议", ["继续在右侧选择游戏，可查看单个扩展集内容。", "在首页功能2中只会使用会社集合，不读取右侧单个游戏选择。"]),
            ],
        )

    def extension_file_count_lines(self, entry: ExtensionEntry) -> list[str]:
        validation = self.extension_validation_info(entry.path)
        return [
            f"rules.int：{'存在' if entry.has_rules else '缺少'}",
            f"manifest.int：{'存在' if (entry.path / 'manifest.int').exists() else '缺少'}",
            f"测试结果：{validation.get('restored_files') or '-'} / {validation.get('total_files') or '-'}",
            f"成功率：{validation.get('success_rate') or '未测试'}",
            f"扩展集大小：{validation.get('package_size_kb') or self.publisher_package_size_kb(entry.path)} KB",
        ]

    def parse_validation_report_file(self, report: Path) -> dict[str, str]:
        """Read the human-readable validation report as a fallback source.

        Some installed extension folders may have ValidationReport.txt copied but
        may miss PublisherTestResult.ini or the [Validation] section in
        manifest.int.  The extension detail page should still show the saved
        success rate instead of falling back to "未测试".
        """
        info = {
            "passed": "",
            "passed_text": "",
            "success_rate": "",
            "package_size_kb": "",
            "pass_basis": "",
            "restored_files": "",
            "total_files": "",
            "error": "",
        }
        if not report.exists():
            return info
        content = self.read_text_best_effort(report)
        for raw_line in content.splitlines():
            line = raw_line.strip()
            if not line or ":" not in line:
                continue
            key, value = line.split(":", 1)
            key = key.strip()
            value = value.strip()
            if key == "测试结果":
                lowered = value.lower()
                if value == "通过" or lowered in ("yes", "passed", "pass"):
                    info["passed"] = "yes"
                    info["passed_text"] = "通过"
                elif value == "未通过" or lowered in ("no", "failed", "fail"):
                    info["passed"] = "no"
                    info["passed_text"] = "未通过"
                elif value:
                    info["passed_text"] = value
            elif key == "通过依据":
                info["pass_basis"] = value
            elif key == "扩展集大小":
                info["package_size_kb"] = value.replace("KB", "").replace("kb", "").strip()
            elif key == "成功还原":
                if "/" in value:
                    restored, total = value.split("/", 1)
                    info["restored_files"] = restored.strip()
                    info["total_files"] = total.strip()
            elif key == "成功率":
                info["success_rate"] = value
        return info

    def extension_validation_info(self, path: Path) -> dict[str, str]:
        info = {
            "passed": "",
            "passed_text": "",
            "success_rate": "",
            "package_size_kb": "",
            "pass_basis": "",
            "restored_files": "",
            "total_files": "",
            "error": "",
            "source": "",
        }

        def apply_values(values: dict[str, str], source_name: str) -> None:
            passed = values.get("passed", "").lower()
            if passed and not info["passed"]:
                info["passed"] = passed
            if not info["passed_text"]:
                if passed == "yes":
                    info["passed_text"] = "通过"
                elif passed == "no":
                    info["passed_text"] = "未通过"
                elif values.get("passed_text"):
                    info["passed_text"] = values.get("passed_text", "")
            for target_key, *source_keys in (
                ("success_rate", "success_rate", "successrate"),
                ("package_size_kb", "package_size_kb", "packagesizekb"),
                ("pass_basis", "pass_basis", "passbasis"),
                ("restored_files", "restored_files", "restoredfiles"),
                ("total_files", "total_files", "totalfiles"),
                ("error", "restore_error", "error"),
            ):
                if info[target_key]:
                    continue
                for source_key in source_keys:
                    value = values.get(source_key, "")
                    if value:
                        info[target_key] = value
                        break
            if not info["source"] and any(value for key, value in values.items() if key != "source"):
                info["source"] = source_name

        result_ini = path / "PublisherTestResult.ini"
        if result_ini.exists():
            values: dict[str, str] = {}
            for raw_line in self.read_text_best_effort(result_ini).splitlines():
                if "=" not in raw_line:
                    continue
                key, value = raw_line.split("=", 1)
                values[key.strip().lower()] = value.strip()
            apply_values(values, "PublisherTestResult.ini")

        manifest_int = path / "manifest.int"
        if manifest_int.exists():
            values = self.read_int_section(manifest_int, "Validation")
            if values:
                apply_values(values, "manifest.int [Validation]")

        manifest_ini = path / "manifest.ini"
        if manifest_ini.exists():
            parser = self.preserve_config_case(configparser.ConfigParser())
            try:
                parser.read(manifest_ini, encoding="utf-8-sig")
            except configparser.Error:
                parser = None
            if parser and parser.has_section("Validation"):
                values = {key.lower(): value.strip() for key, value in parser.items("Validation", raw=True)}
                apply_values(values, "manifest.ini [Validation]")

        report_values = self.parse_validation_report_file(path / "ValidationReport.txt")
        if any(report_values.values()):
            apply_values(report_values, "ValidationReport.txt")

        if not info["success_rate"] and info["restored_files"] and info["total_files"]:
            info["success_rate"] = self.restore_rate_text(info["restored_files"], info["total_files"])
        if not info["passed_text"]:
            if info["passed"] == "yes":
                info["passed_text"] = "通过"
            elif info["passed"] == "no":
                info["passed_text"] = "未通过"
            elif info["success_rate"]:
                info["passed_text"] = "通过"  # 有成功率数据说明已完成测试
        return info

    def validation_section_exists(self, path: Path) -> bool:
        if not path.exists():
            return False
        if path.suffix.lower() == ".int":
            return "validation" in read_int_sections_best_effort(path)
        parser = self.preserve_config_case(configparser.ConfigParser())
        try:
            parser.read(path, encoding="utf-8-sig")
        except configparser.Error:
            return False
        return parser.has_section("Validation")

    def strip_validation_section_from_ini(self, path: Path) -> str | None:
        """去掉 manifest.int 中的 [Validation] 段，返回处理后的文本。"""
        if not path.exists():
            return None
        try:
            text = path.read_text(encoding="utf-8-sig")
        except OSError:
            return None
        lines = text.splitlines(keepends=True)
        result: list[str] = []
        in_validation = False
        for line in lines:
            stripped = line.strip()
            if stripped.lower() == "[validation]":
                in_validation = True
                continue  # 不保留 [Validation] 行
            if in_validation:
                # 下个段落开始 → 退出 validation
                if stripped.startswith("[") and stripped.endswith("]") and not stripped.startswith("[["):
                    in_validation = False
                    # 这个新段落的 [xxx] 行要保留
                    result.append(line)
                # 否则还在 [Validation] 范围内，跳过
                continue
            result.append(line)
        return "".join(result)

    def extension_detail_lines_for_path(self, path: Path, brand: str = "", game: str = "") -> list[tuple[str, list[str]]]:
        metadata = self.read_draft_metadata(path)
        brand = brand or metadata.get("brand", "")
        game = game or metadata.get("game", "")
        manifest = path / "manifest.int"
        rules = path / "rules.int"
        hash_seed = ""
        validation = self.extension_validation_info(path) if path.exists() else {}
        success_rate = validation.get("success_rate", "")
        pass_basis = validation.get("pass_basis", "")
        passed_text = validation.get("passed_text", "")
        restored_files = validation.get("restored_files", "")
        total_files = validation.get("total_files", "")
        validation_error = validation.get("error", "")
        validation_source = validation.get("source", "")
        package_size_kb = validation.get("package_size_kb", "") or (self.publisher_package_size_kb(path) if path.exists() else "")
        rules_ok = False
        rules_issue = "缺少"
        has_voice_pattern = False
        if rules.exists():
            rules_ok, rules_issue = self.rules_int_integrity(rules)
            values = self.read_rules_meta(rules)
            hash_seed = values.get("hashseed", "")
            has_voice_pattern = bool(values.get("voicepattern", ""))
        if not hash_seed and manifest.exists():
            hash_seed = ExtensionCatalog.read_int_meta(manifest).get("hashseed", "")
        content = [
            f"rules.int：{'存在' if rules.exists() else '缺少'}",
            f"manifest.int：{'存在' if manifest.exists() else '缺少'}",
            f"测试记录：{'已写入 manifest.int [Validation]' if validation_source else '未找到'}",
            f"rules.int 状态：{'正常' if rules_ok else rules_issue}",
            f"VoicePattern：{'存在' if has_voice_pattern else '无'}",
            f"扩展集大小：{package_size_kb or '-'} KB",
        ]
        validation_lines = [
            f"测试状态：{passed_text or '未测试'}",
            f"测试结果：{restored_files or '-'} / {total_files or '-'}",
            f"成功率：{success_rate or '未测试'}",
            f"通过依据：{pass_basis or "制作完成即可使用"}",
            f"测试问题：{validation_error or '无'}",
            f"读取来源：{validation_source or '未找到测试记录'}",
        ]
        return [
            ("当前扩展集", [
                f"会社：{brand or '未填写'}",
                f"游戏：{game or '未填写'}",
                f"贡献者：{metadata.get('author', '') or '未填写'}",
                f"版本：{metadata.get('version', '') or '未填写'}",
                f"日期：{metadata.get('date', '') or '未填写'}",
                f"留言：{metadata.get('summary', '') or '未填写'}",
                f"路径：{path}",
                f"HashSeed：{hash_seed or '(未设置)'}",
            ]),
            ("测试记录", validation_lines),
            ("包含内容", content),
        ]

    def show_extension_detail(self, entry: ExtensionEntry) -> None:
        self.set_info_cards(
            "扩展集详情",
            self.extension_detail_lines_for_path(entry.path, entry.brand, entry.game) + [
                ("可用于", ["首页功能1：该作扩展集提取。", "首页功能2：该会社集合撞新作时，会作为会社集合的一部分参与合并。"]),
            ],
        )

    def show_extension_description(self) -> None:
        self.set_info_cards(
            "扩展集留言",
            [
                ("扩展集是什么", ["扩展集用于保存某个会社或某个游戏已经整理好的 Hash、文件名、目录名和规则信息。", "它的目标是提高 CxdecV2 资源名还原率，避免每次都完全依赖动态 Hash 收集。"]),
                ("首页中的用途", ["该作扩展集提取：使用 Extensions\\会社\\游戏 下的单个游戏扩展集。", "该会社集合撞新作：只选择会社，合并该会社下所有游戏扩展集，用旧作命名规律去撞新作。", "传统动态模式：不依赖扩展集，先动态提取再动态收集 Hash。"]),
                ("推荐目录结构", ["Extensions\\会社\\游戏\\rules.int", "Extensions\\会社\\游戏\\manifest.int", "正式扩展集只发布规则包，不包含 StaticHash_Output 这类运行时输出。"]),
            ],
        )

    def user_flow_name(self, mode: int | None = None) -> str:
        mode = self.current_user_mode if mode is None else mode
        return USER_FLOW_DEFINITIONS.get(mode, USER_FLOW_DEFINITIONS[1])["name"]

    def user_flow_detail_text(self, mode: int | None = None) -> str:
        mode = self.current_user_mode if mode is None else mode
        data = USER_FLOW_DEFINITIONS.get(mode, USER_FLOW_DEFINITIONS[1])
        return (
            f"{mode}. {data['name']}\n\n"
            f"用途：{data['target']}\n\n"
            f"操作步骤：\n{data['steps']}\n\n"
            f"{data['output']}\n"
            f"说明：{data['next']}"
        )

    def _show_user_extension_area_for_mode(self, mode: int) -> None:
        if hasattr(self, "user_extension_area_stack"):
            self.user_extension_area_stack.setCurrentIndex(1 if mode == 3 else 0)
        if hasattr(self, "user_extension_group"):
            self.user_extension_group.setEnabled(mode != 3)
            self.user_extension_group.setTitle("扩展集选择")
        if hasattr(self, "user_advanced_group"):
            self.user_advanced_group.setEnabled(mode == 3)
            self.user_advanced_group.setTitle("高级操作")

    def update_user_flow_detail(self) -> None:
        if hasattr(self, "user_flow_action_stack"):
            self.user_flow_action_stack.setCurrentIndex(self.current_user_mode - 1)
        if hasattr(self, "user_flow_group") and hasattr(self.user_flow_group, "setTitle"):
            self.user_flow_group.setTitle("")
        self._show_user_extension_area_for_mode(self.current_user_mode)
        self.update_user_extension_interaction()
        for mode, button in self.user_flow_buttons.items():
            button.setChecked(mode == self.current_user_mode)
        self.show_user_overview()

    def set_active_user_flow(self, mode: int, update_info: bool = True, log_selection: bool = False) -> None:
        self.current_user_mode = mode
        if hasattr(self, "user_flow_action_stack"):
            self.user_flow_action_stack.setCurrentIndex(mode - 1)
        if hasattr(self, "user_flow_group") and hasattr(self.user_flow_group, "setTitle"):
            self.user_flow_group.setTitle("")
        self._show_user_extension_area_for_mode(mode)
        self.update_user_extension_interaction()
        for item_mode, button in self.user_flow_buttons.items():
            button.setChecked(item_mode == mode)
        if update_info:
            self.update_user_flow_detail()
        # 首页功能切换只刷新右侧仪表盘，不再写入详细日志；日志只保留真正执行过的动作。

    def select_user_flow(self, mode: int) -> None:
        self.last_workspace = None
        self.set_active_user_flow(mode, update_info=True, log_selection=False)

    def play_notice_sound(self, success: bool = True) -> None:
        """播放一个轻量提示音；Windows 用系统提示音，其它平台退回 Qt beep。"""
        try:
            if os.name == "nt":
                import winsound

                winsound.MessageBeep(winsound.MB_OK if success else winsound.MB_ICONHAND)
            else:
                QApplication.beep()
        except Exception:
            try:
                QApplication.beep()
            except Exception:
                pass

    def show_step_result_dialog(
        self,
        *,
        mode: int,
        title: str,
        output_dir: Path | None,
        code: int,
        output: str,
        next_step: str = "",
    ) -> None:
        """主流程按钮执行后弹出二级结果窗口，避免用户只能在右侧日志里猜结果。"""
        values = self.parse_cli_values(output)
        success = code == 0
        self.play_notice_sound(success)

        lines: list[str] = [
            f"功能：User\\{mode}（{self.user_flow_name(mode)}）",
            f"操作：{title}",
            f"结果：{'成功' if success else '失败'}",
        ]
        if output_dir is not None:
            lines.append(f"输出目录：{output_dir}")
            if output_dir.exists():
                lines.append("目录状态：已生成")
            else:
                lines.append("目录状态：未找到，请查看详细日志")
        if values.get("STATIC_HASH_OUTPUT"):
            lines.append(f"静态 Hash 输出：{values['STATIC_HASH_OUTPUT']}")
        if values.get("WORKSPACE"):
            lines.append(f"工作区：{values['WORKSPACE']}")
        if values.get("RESTORE") == "completed":
            total = values.get("TOTAL_FILES", "0")
            restored = values.get("RESTORED_FILES", "0")
            lines.append(f"总还原率：{restored} / {total}，成功率 {self.restore_rate_text(restored, total)}")
            # 各目录还原情况
            if output_dir is not None and output_dir.exists():
                report_path = output_dir / "RestoreReport.txt"
                packages = self.parse_restore_package_details(report_path)
                if packages:
                    lines.append("")
                    lines.append("--- 各目录还原情况 ---")
                    for name, r, t, rate in packages:
                        lines.append(f"  {name}：{r}/{t}（{rate}）")
        if values.get("NEXT_STEP"):
            lines.append(f"核心建议：{values['NEXT_STEP']}")
        if next_step:
            lines.append(f"下一步：{next_step}")
        if not success:
            first_line = output.strip().splitlines()[0] if output.strip() else "无输出"
            lines.append(f"错误摘要：{first_line}")

        box = QMessageBox(self)
        box.setWindowTitle(APP_TITLE)
        box.setIcon(QMessageBox.Information if success else QMessageBox.Warning)
        box.setText(f"{title}{'完成' if success else '失败'}")
        box.setInformativeText("\n".join(lines))
        open_button = None
        if output_dir is not None and output_dir.exists():
            open_button = box.addButton("打开输出目录", QMessageBox.ActionRole)
        box.addButton("确定", QMessageBox.AcceptRole)
        box.exec()
        if open_button is not None and box.clickedButton() is open_button:
            self.open_existing_path(output_dir)

    def _show_publisher_test_popup(self, passed: bool, restored: int, total: int, pass_basis: str) -> None:
        """制作扩展集测试完成后弹出结果窗口，包含各目录还原情况，仅供参考不拦截安装。"""
        rate = restored / max(total, 1) * 100
        lines: list[str] = [
            f"测试结果：{restored} / {total}（成功率 {rate:.2f}%）",
            "",
            "说明：以上数据仅供参考，是否安装由你决定。",
        ]
        # 各目录还原情况
        source = self.last_publisher_source_workspace
        if source:
            report_path = source / "Restored_Extractor_Output" / "RestoreReport.txt"
            packages = self.parse_restore_package_details(report_path)
            if packages:
                lines.append("")
                lines.append("--- 各目录还原情况 ---")
                for name, r, t, rate_txt in packages:
                    lines.append(f"  {name}：{r}/{t}（{rate_txt}）")

        box = QMessageBox(self)
        box.setWindowTitle(APP_TITLE)
        box.setIcon(QMessageBox.Information)
        box.setText("扩展集测试结果（仅供参考）")
        box.setInformativeText("\n".join(lines))
        box.addButton("确定", QMessageBox.AcceptRole)
        box.exec()

    def execute_current_user_flow(self) -> None:
        if self.current_user_mode in (1, 2):
            self.run_user_flow_core(self.current_user_mode, restore=False)
        else:
            self.launch_dynamic("dynamic-extract")

    def select_game(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "选择游戏主程序", "", "Executable (*.exe);;All files (*.*)")
        if not path:
            return
        self.game_path.setText(path)
        if not self.publisher_game_name.text().strip():
            self.publisher_game_name.setText(Path(path).parent.name)
        self.log(f"已选择游戏：{path}")
        self.refresh_current_page_info()

    def game_dir(self) -> Path | None:
        value = self.game_path.text().strip()
        if not value:
            QMessageBox.information(self, APP_TITLE, "请先选择游戏主程序。")
            return None
        return Path(value).parent

    def prepare_user_workspace(self, mode: int) -> None:
        self.run_user_flow_core(mode, restore=False)

    def run_user_flow_core(self, mode: int, restore: bool) -> None:
        game_dir = self.game_dir()
        if not game_dir:
            return
        args = ["--mode", f"user{mode}", "--game", self.game_path.text()]
        if mode == 1:
            entry = self.current_user_extension()
            if not entry:
                QMessageBox.information(self, APP_TITLE, "请先在扩展集选择中选择一个游戏扩展集。")
                return
            rules = entry.path / "rules.int"
            if rules.exists() and not self.ensure_draft_hash_seed(rules):
                QMessageBox.information(self, APP_TITLE, "当前扩展集缺少 HashSeed，且未能从已有日志自动回填。请先完成动态 Hash/静态 Hash 流程，或修正扩展集 rules.int。")
                return
            args += ["--extension", str(entry.path)]
        elif mode == 2:
            brand = self.current_user_brand()
            if not brand:
                QMessageBox.information(self, APP_TITLE, "请先在扩展集选择中选择会社。")
                return
            args += ["--brand", brand, "--extensions-root", str(self.paths.extensions_dir)]
        if restore:
            args.append("--restore")

        root = WorkbenchPaths.user_workspace(game_dir, mode)
        before_children = self.existing_child_names(root)
        required_dir = "Restored_Extractor_Output" if restore else "StaticHash_Output"
        self.create_user_step_dirs(root, [required_dir])
        self.last_workspace = root
        self.current_user_mode = mode
        self.last_restored_dir = root / "Restored_Extractor_Output" if restore else None
        self.last_restore_report = None
        self.set_active_user_flow(mode, update_info=False, log_selection=False)

        action_text = "还原资源名" if restore else "生成静态 Hash 表"
        self.log(f"User\\{mode} {self.user_flow_name(mode)}：{action_text}，仅预创建 {required_dir}")
        code, output = self.runner.run(args)
        self.remove_unwanted_empty_user_dirs(root, before_children, {required_dir})
        self.show_cli_summary(code, output)
        next_step = "点击步骤2，提取动态 XP3 资源。" if not restore else "查看还原报告，确认还原结果。"
        self.show_step_result_dialog(
            mode=mode,
            title=action_text,
            output_dir=root / required_dir,
            code=code,
            output=output,
            next_step=next_step,
        )

    def open_restore_ui(self, mode: int) -> None:
        game_dir = self.game_dir()
        if not game_dir:
            return

        workspace = WorkbenchPaths.user_workspace(game_dir, mode)
        self.create_user_step_dirs(workspace, ["Restored_Extractor_Output"])
        self.last_workspace = workspace
        self.current_user_mode = mode
        self.log(f"User\\{mode} {self.user_flow_name(mode)}：打开原生资源名还原窗口，工作区 {workspace}")
        self.start_detached_core_window([
            "--mode", "restore-ui",
            "--workspace", str(workspace),
            "--workers", "4",
        ], f"User\\{mode} 资源名还原窗口")

    def launch_dynamic(self, mode: str) -> None:
        self.launch_dynamic_for_mode(self.current_user_mode, mode)

    def launch_dynamic_for_mode(self, user_mode: int, mode: str) -> None:
        game_dir = self.game_dir()
        if not game_dir:
            return
        workspace = WorkbenchPaths.user_workspace(game_dir, user_mode)
        required_dir = self.dynamic_output_dir_name(mode)

        if not self.confirm_dynamic_reinject(mode, workspace):
            return

        before_children = self.existing_child_names(workspace)
        self.create_user_step_dirs(workspace, [required_dir])
        self.last_workspace = workspace
        self.current_user_mode = user_mode
        if required_dir == "Extractor_Output":
            self.last_restored_dir = None
        if self.last_restore_report and self.last_restore_report.parent.parent != workspace:
            self.last_restore_report = None
        self.log(f"User\\{user_mode} {self.user_flow_name(user_mode)}：准备重新加载动态模块 {self.dynamic_mode_title(mode)}，仅预创建 {required_dir}")

        # ---- CxdecDynamicHashCollector C++ GUI 集成 ----
        if mode == "dynamic-stringhash":
            collector_exe = self.paths.app_dir / "core" / "CxdecDynamicHashCollector.exe"
            if collector_exe.exists():
                # 尝试查找已有收集器窗口（可能被"退出"隐藏了）
                existing_hwnd = None
                try:
                    user32 = ctypes.windll.user32
                    existing_hwnd = user32.FindWindowW("CxdecHashCollectorClass", None)
                except Exception as e:
                    self.log(f"[步骤2] FindWindow 异常（不影响后续启动）：{e}")

                if existing_hwnd:
                    try:
                        user32.ShowWindow(existing_hwnd, 1)  # SW_SHOWNORMAL
                        user32.SetForegroundWindow(existing_hwnd)
                        self.log("[步骤2] 检测到已有收集器窗口，已恢复显示")
                        return
                    except Exception as e:
                        self.log(f"[步骤2] 恢复窗口失败（将启动新进程）：{e}")

                module_dll = self.paths.app_dir / "core" / "CxdecStringDumper.dll"
                corecli_exe = self.paths.core_cli
                args = [
                    str(collector_exe),
                    "--game", self.game_path.text(),
                    "--workdir", str(workspace),
                    "--module", str(module_dll),
                    "--corecli", str(corecli_exe),
                ]
                self.log(f"[步骤2] 启动 C++ 动态 Hash 收集器：{' '.join(args)}")

                try:
                    subprocess.Popen(args, creationflags=subprocess.CREATE_NO_WINDOW)
                    self.log("[步骤2] C++ 收集器已启动")
                except Exception as e:
                    self.log(f"[步骤2] 启动 C++ 收集器失败：{e}，回退到旧版 CLI")
                    self._run_old_dynamic(user_mode, mode, workspace, required_dir, before_children)
                return  # ← 成功启动，返回

        # ---- 旧版逻辑（收集器不存在或非 dynamic-stringhash 时 fallback） ----
        self.log("[步骤2] 收集器不可用，使用旧版 CLI 方式")
        self._run_old_dynamic(user_mode, mode, workspace, required_dir, before_children)

    def _run_old_dynamic(
        self,
        user_mode: int,
        mode: str,
        workspace: Path,
        required_dir: str,
        before_children: set[str],
    ) -> None:
        """旧版 CLI 动态模块加载（同步阻塞，fallback 路径）"""
        code, output = self.runner.run(["--mode", mode, "--game", self.game_path.text(), "--output-root", str(workspace)])
        self.remove_unwanted_empty_user_dirs(workspace, before_children, {required_dir})
        self.remember_dynamic_session(mode, workspace, output)
        self.show_cli_summary(code, output)

    def confirm_dynamic_reinject(self, mode: str, workspace: Path) -> bool:
        """动态模块切换前确认旧游戏进程已经释放，避免 DLL 仍停留在旧模块。"""
        session = self.active_dynamic_session
        old_game = str(session.get("game", "")) if session else self.game_path.text()
        old_workspace = str(session.get("workspace", "")) if session else ""
        old_mode = str(session.get("mode", "")) if session else ""
        pid_value = session.get("pid", "") if session else ""

        try:
            pid = int(pid_value)
        except (TypeError, ValueError):
            pid = 0

        current_game = self.game_path.text()
        game_pids = self.find_process_ids_by_executable(current_game)
        if pid and self.is_pid_running(pid) and pid not in game_pids:
            game_pids.append(pid)

        if old_game and old_game != current_game:
            game_pids = [pid] if pid and self.is_pid_running(pid) else []

        if not game_pids:
            return True

        message = (
            "当前目标游戏仍在运行，可能已经加载过旧动态模块。\n\n"
            f"旧模块：{self.dynamic_mode_title(old_mode)}\n"
            f"新模块：{self.dynamic_mode_title(mode)}\n"
            f"旧工作区：{old_workspace}\n"
            f"新工作区：{workspace}\n\n"
            f"将结束进程：{', '.join(str(item) for item in game_pids)}\n\n"
            "如果不重新注入，新的 Hash/XP3/Key 模块可能不会生效。\n"
            "是否结束旧游戏进程，并继续重新加载当前模块？"
        )

        box = QMessageBox(self)
        box.setIcon(QMessageBox.Warning)
        box.setWindowTitle(APP_TITLE)
        box.setText(message)
        continue_button = box.addButton("结束旧进程并重新注入", QMessageBox.AcceptRole)
        cancel_button = box.addButton("取消，我手动关闭", QMessageBox.RejectRole)
        box.setDefaultButton(continue_button)
        box.exec()

        if box.clickedButton() is not continue_button:
            self.log("已取消动态模块切换：请关闭旧游戏进程后再重新点击当前步骤。")
            return False

        if not self.terminate_game_processes(current_game, game_pids):
            QMessageBox.warning(self, APP_TITLE, "未能结束旧游戏进程。请手动关闭游戏后再试。")
            return False

        self.log(f"已结束旧游戏进程，准备重新注入 {self.dynamic_mode_title(mode)}。")
        self.active_dynamic_session = None
        return True

    def remember_dynamic_session(self, mode: str, workspace: Path, output: str) -> None:
        values = self.parse_cli_values(output)
        pid_text = values.get("PROCESS_ID", "") or values.get("进程 ID", "")
        try:
            pid = int(pid_text)
        except (TypeError, ValueError):
            pid = 0
        if pid <= 0:
            return
        self.active_dynamic_session = {
            "game": self.game_path.text(),
            "workspace": str(workspace),
            "mode": mode,
            "pid": pid,
        }

    def dynamic_mode_title(self, mode: str) -> str:
        mapping = {
            "dynamic-extract": "加载解包模块",
            "dynamic-stringhash": "加载字符串 Hash 提取模块",
            "dynamic-keydump": "加载 Key 提取模块",
        }
        return mapping.get(mode, mode or "未知动态模块")

    def is_pid_running(self, pid: int) -> bool:
        if pid <= 0:
            return False
        if os.name == "nt":
            try:
                completed = subprocess.run(
                    ["tasklist", "/FI", f"PID eq {pid}", "/FO", "CSV", "/NH"],
                    text=True,
                    capture_output=True,
                    encoding="utf-8",
                    errors="replace",
                    timeout=5,
                    creationflags=subprocess.CREATE_NO_WINDOW,
                )
            except Exception:
                return False
            return str(pid) in completed.stdout
        try:
            os.kill(pid, 0)
            return True
        except OSError:
            return False

    def terminate_pid(self, pid: int) -> bool:
        if pid <= 0:
            return True
        if os.name == "nt":
            try:
                completed = subprocess.run(
                    ["taskkill", "/PID", str(pid), "/T", "/F"],
                    text=True,
                    capture_output=True,
                    encoding="utf-8",
                    errors="replace",
                    timeout=10,
                    creationflags=subprocess.CREATE_NO_WINDOW,
                )
            except Exception as exc:
                self.log(f"结束旧进程失败：{exc}")
                return False
            if completed.stdout:
                self.log(completed.stdout.rstrip())
            if completed.stderr:
                self.log(completed.stderr.rstrip())
            return completed.returncode == 0 or not self.is_pid_running(pid)
        try:
            os.kill(pid, 15)
            return True
        except OSError as exc:
            self.log(f"结束旧进程失败：{exc}")
            return False

    def find_process_ids_by_executable(self, executable: str) -> list[int]:
        path = executable.strip()
        if not path or os.name != "nt":
            return []
        image_name = Path(path).name
        script = (
            "$target = [System.IO.Path]::GetFullPath($args[0]);"
            "$image = $args[1];"
            "Get-CimInstance Win32_Process | "
            "Where-Object { "
            "($_.ExecutablePath -and ([System.IO.Path]::GetFullPath($_.ExecutablePath) -ieq $target)) "
            "-or ($_.Name -ieq $image) "
            "} | "
            "ForEach-Object { $_.ProcessId }"
        )
        try:
            completed = subprocess.run(
                ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script, path, image_name],
                text=True,
                capture_output=True,
                encoding="utf-8",
                errors="replace",
                timeout=8,
                creationflags=subprocess.CREATE_NO_WINDOW,
            )
        except Exception as exc:
            self.log(f"枚举目标游戏进程失败：{exc}")
            return []
        pids: list[int] = []
        for line in completed.stdout.splitlines():
            try:
                value = int(line.strip())
            except ValueError:
                continue
            if value > 0 and value not in pids:
                pids.append(value)
        if not pids and image_name:
            try:
                fallback = subprocess.run(
                    ["tasklist", "/FI", f"IMAGENAME eq {image_name}", "/FO", "CSV", "/NH"],
                    text=True,
                    capture_output=True,
                    encoding="utf-8",
                    errors="replace",
                    timeout=5,
                    creationflags=subprocess.CREATE_NO_WINDOW,
                )
            except Exception:
                fallback = None
            if fallback:
                for line in fallback.stdout.splitlines():
                    parts = [part.strip().strip('"') for part in line.split(",")]
                    if len(parts) >= 2 and parts[0].lower() == image_name.lower():
                        try:
                            value = int(parts[1])
                        except ValueError:
                            continue
                        if value > 0 and value not in pids:
                            pids.append(value)
        return pids

    def terminate_game_processes(self, executable: str, pids: list[int]) -> bool:
        all_pids = list(dict.fromkeys([pid for pid in pids if pid > 0] + self.find_process_ids_by_executable(executable)))
        if not all_pids:
            return True
        ok = True
        for pid in all_pids:
            ok = self.terminate_pid(pid) and ok
        remaining = [pid for pid in self.find_process_ids_by_executable(executable) if self.is_pid_running(pid)]
        if remaining:
            self.log(f"仍检测到目标游戏进程未退出：{', '.join(str(pid) for pid in remaining)}")
            return False
        return ok

    def start_detached_core_window(self, args: list[str], description: str) -> None:
        process = QProcess(self)
        process.setProgram(str(self.paths.core_cli))
        process.setArguments(args)
        process.setProcessChannelMode(QProcess.MergedChannels)
        self.restore_processes.append(process)

        def finished(code: int, _status: QProcess.ExitStatus, proc: QProcess = process) -> None:
            output = bytes(proc.readAllStandardOutput()).decode("utf-8", errors="replace")
            if output.strip():
                self.log(output.strip())
            self.log(f"{description} 已关闭，退出码 {code}。")
            if proc in self.restore_processes:
                self.restore_processes.remove(proc)
            proc.deleteLater()

        process.finished.connect(finished)
        process.start()
        if not process.waitForStarted(3000):
            self.log(f"{description} 启动失败：{process.errorString()}")
            QMessageBox.warning(self, APP_TITLE, f"{description} 启动失败：{process.errorString()}")
            if process in self.restore_processes:
                self.restore_processes.remove(process)
            process.deleteLater()


    def open_legacy_loader_dialog(self) -> None:
        existing = getattr(self, "_legacy_loader_dialog", None)
        if existing is not None and existing.isVisible():
            existing.raise_()
            existing.activateWindow()
            return
        self._legacy_loader_dialog = LegacyLoaderDialog(self)
        self._legacy_loader_dialog.show()

    def run_legacy_loader_action(self, action: str) -> None:
        """执行旧版 Loader 五个按钮对应的新版动作。"""
        legacy_mode = 3
        self.set_active_user_flow(legacy_mode, update_info=False, log_selection=False)
        if action == "extract":
            self.launch_dynamic_for_mode(legacy_mode, "dynamic-extract")
        elif action == "stringhash":
            self.launch_dynamic_for_mode(legacy_mode, "dynamic-stringhash")
        elif action == "keydump":
            self.launch_dynamic_for_mode(legacy_mode, "dynamic-keydump")
        elif action == "restore":
            self.open_restore_ui(legacy_mode)
        elif action == "static-hash":
            self.run_traditional_static_hash_mapping()
        else:
            QMessageBox.warning(self, APP_TITLE, f"未知原版功能：{action}")

    def run_traditional_static_hash_mapping(self) -> None:
        """传统动态模式下，按原版 Loader 的“静态生成Hash映射”执行。

        这个入口不能再走 user3 的流程准备逻辑。user3 的普通步骤 1/2/3 是
        “动态 XP3 -> 动态 Hash -> 还原”，而原版 Loader 的“静态生成Hash映射”
        是一个独立高级操作：读取当前 User\\3 里的前提输出，生成 User\\3\\StaticHash_Output。
        """
        game_dir = self.game_dir()
        if not game_dir:
            return

        workspace = WorkbenchPaths.user_workspace(game_dir, 3)
        if not workspace:
            return

        extractor_output = workspace / "Extractor_Output"
        string_hash_output = workspace / "StringHashDumper_Output"
        missing: list[str] = []
        if not self.has_output_content(extractor_output):
            missing.append(f"- 缺少或为空：{extractor_output}")
        if not self.has_output_content(string_hash_output):
            missing.append(f"- 缺少或为空：{string_hash_output}")

        if missing:
            QMessageBox.information(
                self,
                APP_TITLE,
                "传统动态模式的“静态生成Hash映射”需要先准备好动态 XP3 输出和动态 Hash 输出。\n\n"
                + "\n".join(missing),
            )
            return

        before_children = self.existing_child_names(workspace)
        self.create_user_step_dirs(workspace, ["StaticHash_Output"])
        self.last_workspace = workspace
        self.current_user_mode = 3
        self.log("User\\3 传统动态模式：执行原版 Loader 的静态生成Hash映射，仅预创建 StaticHash_Output")
        code, output = self.runner.run([
            "--mode",
            "static-hash",
            "--game",
            self.game_path.text(),
            "--output-root",
            str(workspace),
        ])
        self.remove_unwanted_empty_user_dirs(workspace, before_children, {"StaticHash_Output"})
        self.show_cli_summary(code, output)

    def inspect_user_flow_status(self) -> None:
        game_dir = self.game_dir()
        if not game_dir:
            return

        lines = ["当前信息", "", "首页流程状态", ""]
        for mode in (1, 2, 3):
            workspace = WorkbenchPaths.user_workspace(game_dir, mode)
            static_report = workspace / "StaticHash_Output" / "StaticHashReport.txt"
            restored_report = workspace / "Restored_Extractor_Output" / "RestoreReport.txt"
            extractor_output = workspace / "Extractor_Output"
            dynamic_output = workspace / "StringHashDumper_Output"

            static_ok = static_report.exists()
            restore_ok = restored_report.exists()
            extractor_count = self.count_child_directories(extractor_output)
            dynamic_count = self.count_existing_hash_logs(dynamic_output)

            lines.append(f"User\\{mode}（{self.user_flow_name(mode)}）")
            lines.append(f"  工作区: {workspace}")
            lines.append(f"  静态 Hash: {'已生成' if static_ok else '未生成'}")
            lines.append(f"  动态 XP3 输出目录数: {extractor_count}")
            if mode == 3:
                lines.append(f"  动态 Hash 日志: {dynamic_count}/2")
            lines.append(f"  还原报告: {'已生成' if restore_ok else '未生成'}")
            lines.append("")

        active = self.current_user_mode
        active_workspace = WorkbenchPaths.user_workspace(game_dir, active)
        active_static = active_workspace / "StaticHash_Output" / "StaticHashReport.txt"
        active_restore = active_workspace / "Restored_Extractor_Output" / "RestoreReport.txt"
        active_extractor = active_workspace / "Extractor_Output"
        active_dynamic = active_workspace / "StringHashDumper_Output"

        lines.append("建议下一步")
        lines.append(f"- 当前功能：User\\{active}（{self.user_flow_name(active)}）")
        if active in (1, 2) and not active_static.exists():
            lines.append("- 点击二级页面的“步骤1”，先生成 StaticHash_Output。")
        elif active == 3 and self.count_child_directories(active_extractor) == 0:
            lines.append("- 点击二级页面的“步骤1”，先提取动态 XP3 资源。")
        elif active in (1, 2) and self.count_child_directories(active_extractor) == 0:
            lines.append("- 点击二级页面的“步骤2”，进游戏触发动态 XP3 输出。")
        elif active == 3 and self.count_existing_hash_logs(active_dynamic) < 2:
            lines.append("- 点击二级页面的“步骤2”，进游戏收集动态 Hash。")
        elif not active_restore.exists():
            lines.append("- 点击二级页面的“步骤3”，执行资源名还原。")
        else:
            lines.append("- 当前已有还原报告，可以打开报告或进入制作流程。")

        self.set_info("\n".join(lines))
        self.log(f"已检查首页流程状态：{game_dir}")

    def count_child_directories(self, path: Path) -> int:
        if not path.exists():
            return 0
        return sum(1 for child in path.iterdir() if child.is_dir())

    def count_existing_hash_logs(self, path: Path) -> int:
        if not path.exists():
            return 0
        exact_count = sum(1 for name in ("DirectoryHash.log", "FileNameHash.log") if (path / name).exists())
        if exact_count:
            return exact_count
        # 兼容旧 Loader / 手动拷贝的动态 Hash 输出：有些版本不会固定写成
        # DirectoryHash.log / FileNameHash.log，检查状态时只要目录里有实际文件，就按“已有 Hash 产物”处理。
        return 2 if self.has_output_content(path) else 0

    def has_output_content(self, path: Path) -> bool:
        """判断输出目录是否有实际内容；只检查存在性，不主动创建任何目录。"""
        if not path.exists() or not path.is_dir():
            return False
        try:
            return any(child.is_file() or child.is_dir() for child in path.iterdir())
        except OSError:
            return False

    def current_user_workspace(self) -> Path | None:
        game_dir = self.game_dir()
        if not game_dir:
            return None
        return WorkbenchPaths.user_workspace(game_dir, self.current_user_mode)

    def open_current_workspace(self) -> None:
        workspace = self.current_user_workspace()
        if not workspace:
            return
        self.open_path(workspace)


    def clean_current_workspace(self) -> None:
        workspace = self.current_user_workspace()
        if not workspace:
            return
        if not workspace.exists():
            QMessageBox.information(self, APP_TITLE, f"当前工作区不存在：{workspace}")
            return
        if QMessageBox.question(self, APP_TITLE, f"确定删除当前工作区？\n{workspace}") != QMessageBox.Yes:
            return
        shutil.rmtree(workspace)
        self.last_workspace = None
        self.last_restore_report = None
        self.last_restored_dir = None
        self.set_info(f"当前信息\n\n已清理 User\\{self.current_user_mode} 工作区。")

    def create_user_step_dirs(self, root: Path, names: list[str]) -> None:
        """只创建当前步骤实际需要的目录，避免一进入流程就生成一堆空目录。"""
        root.mkdir(parents=True, exist_ok=True)
        for name in names:
            if name:
                (root / name).mkdir(parents=True, exist_ok=True)

    def dynamic_output_dir_name(self, mode: str) -> str:
        mapping = {
            "dynamic-extract": "Extractor_Output",
            "dynamic-stringhash": "StringHashDumper_Output",
            "dynamic-keydump": "ExtractKey_Output",
        }
        return mapping.get(mode, "Extractor_Output")

    def existing_child_names(self, root: Path) -> set[str]:
        if not root.exists():
            return set()
        return {child.name for child in root.iterdir()}

    def remove_unwanted_empty_user_dirs(self, root: Path, before_children: set[str], keep_names: set[str]) -> None:
        """清掉本次误创建的空目录，但不动执行前就存在的目录和非空目录。"""
        allowed = {
            "Extractor_Output",
            "StaticHash_Output",
            "Restored_Extractor_Output",
            "StringHashDumper_Output",
            "ExtractKey_Output",
        }
        for name in sorted(allowed - keep_names):
            child = root / name
            if name in before_children or not child.is_dir():
                continue
            try:
                child.rmdir()
                self.log(f"已移除本次多余空目录：{child}")
            except OSError:
                # 目录里已有文件时不删除，避免误删核心输出或用户文件。
                pass

    def prepare_publisher_workspace(self) -> None:
        game_dir = self.game_dir()
        if not game_dir:
            return
        publisher_root = WorkbenchPaths.publisher_workspace(game_dir)
        before_children = self.existing_child_names(publisher_root)
        root = publisher_root / "ExtensionDraft"
        root.mkdir(parents=True, exist_ok=True)
        self.log(f"已准备制作扩展集工作区：{root}，仅预创建 ExtensionDraft")
        self.last_workspace = root
        self.last_publisher_draft = root
        self.last_publisher_report = None
        self.last_publisher_test_error = ""
        if not self.custom_publisher_source_workspace:
            self.last_publisher_source_workspace = self.publisher_source_workspace(game_dir)
        self.publisher_metadata_confirmed = False
        args = ["--mode", "publisher-make", "--game", self.game_path.text()]
        brand = self.publisher_brand.text().strip()
        if brand:
            args += ["--brand", brand]
        code, output = self.runner.run(args)
        self.remove_unwanted_empty_publisher_dirs(publisher_root, before_children, {"ExtensionDraft"})
        if code != 0:
            self.show_cli_summary(code, output)
        else:
            values = self.parse_cli_values(output)
            report = values.get("PUBLISHER_REPORT", "")
            if report:
                self.last_publisher_report = Path(report)

    def generate_minimal_extension(self) -> None:
        self.prepare_publisher_workspace()
        draft = self.last_publisher_draft
        if not draft or not draft.exists():
            return
        manifest = draft / "manifest.int"
        rules = draft / "rules.int"
        if not manifest.exists() or not rules.exists():
            return
        self.ensure_draft_hash_seed(rules)
        if not self.validate_rules_int_or_warn(rules, "生成当前游戏扩展集"):
            self.show_publisher_status()
            return
        self.save_rules_int_backup(draft, "生成后")
        metadata = self.publisher_metadata_fields()
        brand, game = self.read_draft_identity(manifest, rules)
        metadata["brand"] = metadata["brand"] or brand
        metadata["game"] = metadata["game"] or game or self.game_path_to_name()
        self.apply_publisher_metadata_fields(metadata)
        if metadata["brand"] and metadata["brand"] != "Unknown":
            self.normalize_draft_metadata(
                manifest,
                rules,
                metadata["brand"],
                metadata["game"],
                metadata["author"],
                metadata["version"],
                metadata["summary"],
                metadata["date"],
            )
        package_bytes = self.publisher_package_size(draft)
        QApplication.beep()
        self.show_publisher_status()
        QMessageBox.information(self, APP_TITLE, "当前游戏扩展集已生成，请确认扩展集信息后再测试。")
        self.log(f"已生成当前游戏扩展集，等待用户确认信息：{draft}")

    def generate_and_install_minimal_extension(self) -> None:
        game_dir = self.game_dir()
        if not game_dir:
            return

        self.prepare_publisher_workspace()
        draft = self.last_publisher_draft
        if not draft or not draft.exists():
            return

        manifest = draft / "manifest.int"
        rules = draft / "rules.int"
        if not manifest.exists() or not rules.exists():
            return

        metadata = self.publisher_metadata_fields()
        if not metadata["brand"] or metadata["brand"] == "Unknown":
            QMessageBox.information(self, APP_TITLE, "请先填写会社名。")
            return
        self.normalize_draft_metadata(
            manifest,
            rules,
            metadata["brand"],
            metadata["game"],
            metadata["author"],
            metadata["version"],
            metadata["summary"],
            metadata["date"],
        )
        self.ensure_draft_hash_seed(rules)
        if not self.validate_rules_int_or_warn(rules, "一键生成并安装"):
            self.show_publisher_status()
            return
        self.save_rules_int_backup(draft, "一键生成后")

        # 直接验证并安装，不再检查体积或成功率
        code, output = self.runner.run([
            "--mode", "user1",
            "--game", self.game_path.text(),
            "--extension", str(draft),
            "--restore",
        ])
        self.show_cli_summary(code, output)
        if code != 0:
            self.log("CLI 验证未通过，但扩展集仍可用于参考。")
        else:
            self.log("CLI 验证通过。")

        self.import_extension_draft()
        if self.last_imported_extension:
            package_bytes = self.publisher_package_size(draft)
            self.set_info(
                "当前信息\n\n"
                "当前游戏扩展集已生成并安装。\n"
                f"安装位置：{self.last_imported_extension}\n"
                f"扩展集大小：{package_bytes} bytes\n"
            )
            self.log(
                "已完成一键生成并安装。"
                f" 大小 {package_bytes} bytes。"
            )

    def test_current_publisher_extension(self) -> None:
        game_dir = self.game_dir()
        if not game_dir:
            return
        draft = self.current_publisher_draft_path()
        if not draft or not draft.exists():
            QMessageBox.information(self, APP_TITLE, "还没有可测试的扩展集。请先点击“生成当前游戏扩展集”。")
            return
        if not self.publisher_metadata_confirmed:
            QMessageBox.information(self, APP_TITLE, "请先点击“编辑当前扩展集信息”，确认并保存信息后再测试。")
            return

        manifest = draft / "manifest.int"
        rules = draft / "rules.int"
        if not manifest.exists() or not rules.exists():
            QMessageBox.information(self, APP_TITLE, "当前扩展集缺少 manifest.int 或 rules.int，暂时不能测试。")
            return

        metadata = self.publisher_metadata_fields()
        if not metadata["brand"] or metadata["brand"] == "Unknown":
            QMessageBox.information(self, APP_TITLE, "请先填写会社名并保存扩展集信息。")
            return
        self.normalize_draft_metadata(
            manifest,
            rules,
            metadata["brand"],
            metadata["game"],
            metadata["author"],
            metadata["version"],
            metadata["summary"],
            metadata["date"],
        )
        if not self.ensure_draft_hash_seed(rules):
            QMessageBox.information(self, APP_TITLE, "当前扩展集缺少 HashSeed，且未能从已有日志自动回填。请先完成动态 Hash/静态 Hash 流程后再测试。")
            return
        if not self.validate_rules_int_or_warn(rules, "测试当前扩展集"):
            self.show_publisher_status()
            return
        self.save_rules_int_backup(draft, "测试前")

        source_workspace = self.publisher_source_workspace(game_dir)
        if not source_workspace or not (source_workspace / "Extractor_Output").exists():
            self.last_publisher_test_error = "没有找到来源 Extractor_Output。请先完成动态 XP3 提取，再测试扩展集。"
            self.show_publisher_status()
            QMessageBox.information(self, APP_TITLE, self.last_publisher_test_error)
            return
        self.last_publisher_source_workspace = source_workspace

        package_bytes = self.publisher_package_size(draft)
        if self.publisher_test_process and self.publisher_test_process.state() != QProcess.NotRunning:
            QMessageBox.information(self, APP_TITLE, "当前扩展集测试窗口仍在运行，请等待它完成后再重新测试。")
            return

        process = QProcess(self)
        process.setProgram(str(self.paths.core_cli))
        process.setArguments([
            "--mode", "publisher-test-ui",
            "--output-root", str(source_workspace),
            "--extension", str(draft),
        ])
        process.setProcessChannelMode(QProcess.MergedChannels)
        self.publisher_test_process = process
        self.publisher_test_result_written = False
        # 隐藏主窗口，避免挡住 C++ 原生测试窗口
        self.showMinimized()
        self.log("已打开原生扩展集测试窗口。")
        result_path = draft / "PublisherTestResult.ini"

        # 清除旧结果，避免 finished 读到残留数据
        try:
            result_path.unlink(missing_ok=True)
        except OSError:
            pass
        # 去掉 manifest.int 中旧的 [Validation] 段，避免 finished 前就误判完成
        manifest_clean = self.strip_validation_section_from_ini(manifest)
        if manifest_clean is not None:
            try:
                manifest.write_text(manifest_clean, encoding="utf-8-sig")
            except OSError:
                pass

        try:
            rules_backup = rules.read_bytes()
        except OSError:
            rules_backup = b""

        def restore_rules_int_backup() -> None:
            if not rules_backup:
                return
            try:
                if rules.read_bytes() != rules_backup:
                    rules.write_bytes(rules_backup)
                    self.log("已恢复 rules.int，避免原生测试窗口把二进制规则包当 INI 覆盖。")
            except OSError as exc:
                self.log(f"恢复 rules.int 失败：{exc}")

        def finished(code: int, _status: QProcess.ExitStatus) -> None:
            output = bytes(process.readAllStandardOutput()).decode("utf-8", errors="replace")
            restore_rules_int_backup()
            restore_rules_int_backup = lambda: None  # 防止重复调用

            # 尝试从 stdout 解析结果
            values = self.parse_cli_values(output)
            # stdout 可能不全（GUI 程序输出不可靠），兜底读结果文件
            if not values.get("RESTORE") and result_path.exists():
                values = self.parse_key_value_file(result_path)

            # 恢复主窗口显示并置前，再弹出测试结果
            self.showNormal()
            self.raise_()
            self.activateWindow()

            if values.get("RESTORE") == "completed":
                self.finish_publisher_extension_test_from_values(
                    values,
                    manifest,
                    draft,
                )
            else:
                self.finish_publisher_extension_test(
                    code,
                    output,
                    manifest,
                    rules,
                    draft,
                )
            self.publisher_test_process = None
            process.deleteLater()

        # 停掉旧轮询（如有）
        if self.publisher_test_poller and self.publisher_test_poller.isActive():
            self.publisher_test_poller.stop()

        self.publisher_test_poller = QTimer(self)
        self.publisher_test_poller.setSingleShot(False)
        self.publisher_test_poller.setInterval(500)

        def poll_cpp_result() -> None:
            if process.state() != QProcess.NotRunning:
                return
            self.publisher_test_poller.stop()

            restore_rules_int_backup()
            restore_rules_int_backup = lambda: None

            self.showNormal()
            self.raise_()
            self.activateWindow()

            values = self.parse_key_value_file(result_path)
            if values.get("RESTORE") == "completed":
                self.finish_publisher_extension_test_from_values(values, manifest, draft)
            else:
                exit_code = process.exitCode()
                output = bytes(process.readAllStandardOutput()).decode("utf-8", errors="replace")
                self.finish_publisher_extension_test(exit_code, output, manifest, rules, draft)
            self.publisher_test_process = None
            process.deleteLater()
            process.deleteLater()

        self.publisher_test_poller.timeout.connect(poll_cpp_result)
        process.start()
        if not process.waitForStarted(3000):
            restore_rules_int_backup()
            self.publisher_test_poller.stop()
            self.publisher_test_process = None
            QMessageBox.warning(self, APP_TITLE, f"扩展集测试窗口启动失败：{process.errorString()}")
            process.deleteLater()
            return
        # 5秒后开始轮询 C++ 是否关闭，给用户留够操作时间
        QTimer.singleShot(5000, self.publisher_test_poller.start)
        return

    def finish_publisher_extension_test(
        self,
        code: int,
        output: str,
        manifest: Path,
        rules: Path,
        draft: Path,
    ) -> None:
        if code != 0:
            self.publisher_test_result_written = True
            self.last_publisher_test_error = self.parse_cli_values(output).get("RESTORE_ERROR", output.strip() or "测试失败。")
            self.write_publisher_validation_result(
                manifest,
                draft,
                {
                    "Passed": "no",
                    "PassBasis": "测试进程返回失败。",
                    "Error": self.last_publisher_test_error,
                },
            )
            self.show_publisher_status()
            self.log("当前扩展集测试失败。")
            QApplication.beep()
            return

        values = self.parse_cli_values(output)
        self.finish_publisher_extension_test_from_values(values, manifest, draft)

    def finish_publisher_extension_test_from_values(
        self,
        values: dict[str, str],
        manifest: Path,
        draft: Path,
    ) -> None:
        if self.publisher_test_result_written:
            return
        self.publisher_test_result_written = True
        self.last_publisher_draft = draft
        self.log("已收到扩展集测试结果，正在写入 manifest.int 和测试结果文件。")
        validated_restored = int(values.get("RESTORED_FILES", "0") or "0")
        validated_total = int(values.get("TOTAL_FILES", "0") or "0")
        package_bytes = self.publisher_package_size(draft)

        # 显示真实测试结果，不拦截安装
        restored_rate = self.restore_rate_text(str(validated_restored), str(validated_total))
        pass_basis = f"成功还原 {validated_restored}/{validated_total}（{restored_rate}）"
        self.write_publisher_validation_result(
            manifest,
            draft,
            {
                "PackageBytes": str(package_bytes),
                "PackageSizeKB": f"{package_bytes / 1024:.1f}",
                "TotalFiles": str(validated_total),
                "RestoredFiles": str(validated_restored),
                "SuccessRate": restored_rate,
                "Passed": "yes",
                "PassBasis": pass_basis,
                "Error": "",
            },
        )
        self.last_publisher_test_error = ""
        QApplication.beep()
        self.last_publisher_draft = draft
        self.show_publisher_status()
        self.log(
            f"当前扩展集测试完成："
            f" 验证 {validated_restored}/{validated_total}，"
            f" 大小 {package_bytes} bytes。"
        )
        self._show_publisher_test_popup(True, validated_restored, validated_total, pass_basis)

    def remove_unwanted_empty_publisher_dirs(self, root: Path, before_children: set[str], keep_names: set[str]) -> None:
        if not root.exists():
            return
        for child in sorted(root.iterdir(), key=lambda p: p.name):
            if child.name in before_children or child.name in keep_names or not child.is_dir():
                continue
            try:
                child.rmdir()
                self.log(f"已移除本次多余制作扩展集空目录：{child}")
            except OSError:
                pass

    def publisher_draft_workspace(self) -> Path | None:
        game_dir = self.game_dir()
        if not game_dir:
            return None
        return WorkbenchPaths.publisher_workspace(game_dir) / "ExtensionDraft"

    def current_publisher_draft_path(self, game_dir: Path | None = None) -> Path | None:
        if self.last_publisher_draft and self.last_publisher_draft.exists():
            return self.last_publisher_draft
        if game_dir is None:
            value = self.game_path.text().strip()
            game_dir = Path(value).parent if value else None
        if not game_dir:
            return None
        root = WorkbenchPaths.publisher_workspace(game_dir) / "ExtensionDraft"
        candidates = []
        if (root / "rules.int").exists():
            candidates.append(root)
        candidates.extend(path.parent for path in root.glob("**/rules.int"))
        candidates = [path for path in candidates if ".bak" not in path.name.lower() and ".bak" not in path.parent.name.lower()]
        if candidates:
            unique_candidates = list(dict.fromkeys(candidates))
            return max(unique_candidates, key=self.publisher_draft_priority)
        return root

    def publisher_draft_priority(self, path: Path) -> tuple[int, int, float]:
        has_validation = self.validation_section_exists(path / "manifest.int") or self.validation_section_exists(path / "manifest.ini")
        has_test_result = (path / "PublisherTestResult.ini").exists()
        try:
            mtime = max((item.stat().st_mtime for item in (path / "rules.int", path / "manifest.int", path / "PublisherTestResult.ini") if item.exists()), default=path.stat().st_mtime)
        except OSError:
            mtime = 0.0
        return (1 if has_validation else 0, 1 if has_test_result else 0, mtime)

    def safe_path_name(self, value: str, fallback: str) -> str:
        name = re.sub(r'[<>:"/\\|?*]+', "_", value.strip())
        name = name.strip(" .")
        return name or fallback

    def restart_publisher_workspace(self) -> None:
        workspace = self.publisher_draft_workspace()
        if not workspace:
            return
        if workspace.exists():
            backup = self.next_backup_path(workspace)
            workspace.rename(backup)
            self.log(f"已备份扩展集草稿：{backup}")
        workspace.mkdir(parents=True, exist_ok=True)
        self.last_publisher_draft = workspace
        self.last_publisher_report = None
        self.set_info(f"当前信息\n\n已重新开始制作流程。\n草稿目录: {workspace}")

    def clean_publisher_workspace(self) -> None:
        workspace = self.publisher_draft_workspace()
        if not workspace:
            return
        if not workspace.exists():
            QMessageBox.information(self, APP_TITLE, f"扩展集草稿不存在：{workspace}")
            return
        if QMessageBox.question(self, APP_TITLE, f"确定删除扩展集草稿？\n{workspace}") != QMessageBox.Yes:
            return
        shutil.rmtree(workspace)
        self.last_publisher_draft = None
        self.last_publisher_report = None
        self.set_info("当前信息\n\n已清理扩展集草稿。")

    def save_current_publisher_metadata(self) -> None:
        draft = self.current_publisher_draft_path()
        if not draft or not draft.exists():
            QMessageBox.information(self, APP_TITLE, "还没有可保存信息的扩展集。请先生成当前游戏扩展集。")
            return

        manifest = draft / "manifest.int"
        rules = draft / "rules.int"
        if not manifest.exists() or not rules.exists():
            QMessageBox.information(self, APP_TITLE, "当前扩展集缺少 manifest.int 或 rules.int，暂时不能保存信息。")
            return

        metadata = self.publisher_metadata_fields()
        if not metadata["brand"] or metadata["brand"] == "Unknown":
            QMessageBox.information(self, APP_TITLE, "请先填写会社名。")
            return

        self.normalize_draft_metadata(
            manifest,
            rules,
            metadata["brand"],
            metadata["game"],
            metadata["author"],
            metadata["version"],
            metadata["summary"],
            metadata["date"],
        )
        self.ensure_draft_hash_seed(rules)
        if not self.validate_rules_int_or_warn(rules, "保存扩展集信息"):
            self.show_publisher_status()
            return
        root = self.publisher_draft_workspace()
        if root:
            target = root / self.safe_path_name(metadata["brand"], "UnknownBrand") / self.safe_path_name(metadata["game"], "UnknownGame")
            if draft.resolve() != target.resolve():
                target.parent.mkdir(parents=True, exist_ok=True)
                if target.exists():
                    backup = self.next_backup_path(target)
                    target.rename(backup)
                    self.log(f"已备份旧制作扩展集目录：{backup}")
                self.copy_publisher_package(draft, target)
                draft = target
        self.last_publisher_draft = draft
        self.publisher_metadata_confirmed = True
        self.show_publisher_status()
        self.log(f"已保存扩展集信息：{draft}")

    def select_publisher_extension_source_directory(self) -> None:
        start_path = self.current_publisher_draft_path() or self.publisher_draft_workspace() or self.paths.extensions_dir
        directory = QFileDialog.getExistingDirectory(self, "选择扩展集来源目录", str(start_path))
        if not directory:
            return
        source = Path(directory)
        candidates = [source]
        candidates.extend(path.parent for path in source.glob("*/*/rules.int"))
        selected = next((path for path in candidates if (path / "rules.int").exists() and (path / "manifest.int").exists()), None)
        if not selected:
            QMessageBox.information(self, APP_TITLE, "所选目录下没有 manifest.int 和 rules.int，不能作为扩展集来源目录。")
            return
        self.last_publisher_draft = selected
        metadata = self.read_draft_metadata(selected)
        self.apply_publisher_metadata_fields(metadata)
        self.publisher_metadata_confirmed = bool(metadata.get("brand") and metadata.get("game"))
        self.last_publisher_test_error = ""
        self.show_publisher_status()
        self.log(f"已选择扩展集来源目录：{selected}")

    def select_publisher_source_workspace(self) -> None:
        game_dir = self.game_dir()
        if not game_dir:
            return
        start = self.publisher_source_workspace(game_dir)
        directory = QFileDialog.getExistingDirectory(self, "选择测试来源工作区", str(start))
        if not directory:
            return
        workspace = Path(directory)
        if not (workspace / "Extractor_Output").exists():
            QMessageBox.information(self, APP_TITLE, "所选目录下没有 Extractor_Output，不能作为测试来源工作区。")
            return
        if not self.workspace_belongs_to_game(workspace, game_dir):
            QMessageBox.information(self, APP_TITLE, "测试来源目录必须属于当前目标游戏目录，不能串用上一个游戏的 User\\1/2/3 输出。")
            return
        self.last_publisher_source_workspace = workspace
        self.custom_publisher_source_workspace = workspace
        self.last_publisher_test_error = ""
        self.show_publisher_status()
        self.log(f"已选择测试来源目录：{workspace}")

    def edit_current_publisher_metadata(self) -> None:
        draft = self.current_publisher_draft_path()
        if not draft or not draft.exists():
            QMessageBox.information(self, APP_TITLE, "还没有可编辑的扩展集。请先生成当前游戏扩展集。")
            return
        metadata = self.read_draft_metadata(draft)
        dialog = PublisherMetadataDialog(self, metadata)
        if dialog.exec() != QDialog.Accepted or not dialog.accepted_metadata:
            return
        metadata.update(dialog.accepted_metadata)
        self.apply_publisher_metadata_fields(metadata)
        self.save_current_publisher_metadata()

    def import_extension_draft(self) -> None:
        draft = self.last_publisher_draft
        if not draft:
            game_dir = self.game_dir()
            if not game_dir:
                return
            draft = self.current_publisher_draft_path(game_dir)

        if not draft.exists():
            QMessageBox.information(self, APP_TITLE, f"找不到扩展集草稿目录：{draft}")
            return

        manifest = draft / "manifest.int"
        rules = draft / "rules.int"

        if not manifest.exists() or not rules.exists():
            QMessageBox.information(self, APP_TITLE, "草稿不完整，至少需要 manifest.int 和 rules.int。")
            return
        self.restore_rules_int_backup_if_needed(draft, "安装前")
        self.ensure_draft_hash_seed(rules)
        if not self.validate_rules_int_or_warn(rules, "安装扩展集"):
            self.show_publisher_status()
            return

        metadata = self.publisher_metadata_fields()
        brand, game = self.read_draft_identity(manifest, rules)
        brand = metadata["brand"] or brand
        if not brand or brand == "Unknown":
            QMessageBox.information(self, APP_TITLE, "请先填写会社名，或在 manifest.int 中写入 Brand。")
            return
        game = metadata["game"] or game or self.game_path_to_name()

        target = self.paths.extensions_dir / brand / game
        if target.exists():
            backup = self.next_backup_path(target)
            target.rename(backup)
            self.log(f"已备份旧扩展集：{backup}")

        self.copy_publisher_package(draft, target)
        self.last_imported_extension = target
        self.reload_extensions()
        self.show_publisher_status()
        self.set_info(
            "当前信息\n\n"
            "扩展集已导入/搭载。\n"
            f"会社: {brand}\n"
            f"游戏: {game}\n"
            f"目录: {target}"
        )
        self.log(f"已导入扩展集：{target}")

    def copy_publisher_package(self, source: Path, target: Path) -> None:
        target.mkdir(parents=True, exist_ok=True)
        # The installed extension library should stay clean: only the runtime
        # extension files are copied.  Test reports remain in ExtensionDraft as
        # build/debug artifacts.  The success-rate data required by the UI is
        # already written into manifest.int [Validation].
        for name in ("manifest.int", "rules.int"):
            src = source / name
            if src.exists():
                shutil.copy2(src, target / name)

    def read_draft_identity(self, manifest: Path, rules: Path) -> tuple[str, str]:
        brand = ""
        game = ""
        manifest_meta = ExtensionCatalog.read_int_meta(manifest)
        rules_meta = ExtensionCatalog.read_int_meta(rules)
        brand = manifest_meta.get("brand", "")
        game = manifest_meta.get("game", "")
        brand = brand or rules_meta.get("brand", "")
        game = game or rules_meta.get("gamedisplayname", "")
        game = game or rules_meta.get("gameid", "")
        return brand, game

    def read_draft_metadata(self, draft: Path | None = None) -> dict[str, str]:
        metadata: dict[str, str] = {
            "brand": "",
            "game": "",
            "author": "",
            "version": "",
            "date": "",
            "summary": "",
        }
        if not draft:
            draft = self.current_publisher_draft_path()
        if not draft:
            return metadata

        for path in (draft / "manifest.int", draft / "rules.int"):
            if not path.exists():
                continue
            values = ExtensionCatalog.read_int_meta(path)
            if not metadata["brand"]:
                metadata["brand"] = values.get("brand", "")
            if not metadata["game"]:
                metadata["game"] = values.get("game", "") or values.get("gamedisplayname", "") or values.get("gameid", "")
            if not metadata["author"]:
                metadata["author"] = values.get("contributor", "") or values.get("author", "")
            if not metadata["version"]:
                metadata["version"] = values.get("version", "")
            file_date = values.get("date", "") or values.get("builddate", "")
            if file_date and not metadata["date"]:
                metadata["date"] = normalize_metadata_date(file_date)
            if not metadata["summary"]:
                metadata["summary"] = values.get("summary", "")
        return metadata

    def apply_publisher_metadata_fields(self, metadata: dict[str, str]) -> None:
        self.publisher_brand.setText(metadata.get("brand", ""))
        self.publisher_game_name.setText(metadata.get("game", ""))
        self.publisher_author.setText(metadata.get("author", ""))
        self.publisher_version.setText(metadata.get("version", "") or "1.0")
        self.publisher_date.setText(normalize_metadata_date(metadata.get("date", "")))
        self.publisher_summary.setText(metadata.get("summary", "填你想填的！"))

    def publisher_metadata_fields(self) -> dict[str, str]:
        return {
            "brand": self.publisher_brand.text().strip(),
            "game": self.publisher_game_name.text().strip() or self.game_path_to_name(),
            "author": self.publisher_author.text().strip(),
            "version": self.publisher_version.text().strip() or "1.0",
            "date": normalize_metadata_date(self.publisher_date.text().strip()),
            "summary": self.publisher_summary.text().strip(),
        }

    def find_hash_seed_for_game(self, game_dir: Path) -> str:
        patterns = ("StaticHashReport.txt", "Universal.log")
        markers = ("HashSeed:", "Hash Seed:", "HashSeed：", "Hash Seed：")
        for pattern in patterns:
            for path in game_dir.rglob(pattern):
                content = self.read_text_best_effort(path)
                for line in content.splitlines():
                    text = line.strip()
                    for marker in markers:
                        if marker in text:
                            seed = text.split(marker, 1)[1].strip()
                            if seed and not seed.startswith("("):
                                return seed
        return ""

    def config_get_ci(self, parser: configparser.ConfigParser, section: str, option: str, fallback: str = "") -> str:
        if not parser.has_section(section):
            return fallback
        for key, value in parser.items(section, raw=True):
            if key.lower() == option.lower():
                return value.strip()
        return fallback

    def preserve_config_case(self, parser: configparser.ConfigParser) -> configparser.ConfigParser:
        parser.optionxform = str
        return parser

    def rules_int_integrity(self, rules: Path) -> tuple[bool, str]:
        """Return whether rules.int is a usable rule package and a short status.

        A valid CXRI3 package starts with raw bytes ``CXRI3\0`` and has a
        complete header + [Meta] text block.  ``UTF-8 BOM + CXRI3`` is a known
        corruption pattern produced when a binary rules file is accidentally
        rewritten as text.
        """
        try:
            data = rules.read_bytes()
        except OSError as exc:
            return False, f"读取失败：{exc}"
        size = len(data)
        if size == 0:
            return False, "空文件"
        if data.startswith(b"\xef\xbb\xbfCXRI"):
            return False, f"疑似被当成文本覆盖，仅 {size} 字节"
        if data.startswith(b"CXRI3\0"):
            if size < 30:
                return False, f"CXRI3 头不完整，仅 {size} 字节"
            meta_size = int.from_bytes(data[6:10], "little", signed=False)
            pattern_size = int.from_bytes(data[10:14], "little", signed=False)
            path_count = int.from_bytes(data[14:18], "little", signed=False)
            path_block_size = int.from_bytes(data[18:22], "little", signed=False)
            _compression = int.from_bytes(data[22:26], "little", signed=False)
            stored_path_block_size = int.from_bytes(data[26:30], "little", signed=False)
            expected = 30 + meta_size + pattern_size + stored_path_block_size
            if expected > size:
                return False, f"CXRI3 数据不完整：需要 {expected} 字节，实际 {size} 字节"
            meta = data[30:30 + meta_size]
            if b"[Meta]" not in meta:
                return False, "CXRI3 缺少 [Meta]"
            if b"HashSeed=" not in meta:
                return False, "CXRI3 缺少 HashSeed"
            if path_count == 0 and pattern_size == 0 and path_block_size == 0:
                return False, "CXRI3 没有任何规则或路径"
            return True, f"正常（{size / 1024:.1f} KB）"
        if data.startswith(b"CXRI2\0"):
            if size < 22:
                return False, f"CXRI2 头不完整，仅 {size} 字节"
            meta_size = int.from_bytes(data[6:10], "little", signed=False)
            pattern_size = int.from_bytes(data[10:14], "little", signed=False)
            path_block_size = int.from_bytes(data[18:22], "little", signed=False)
            expected = 22 + meta_size + pattern_size + path_block_size
            if expected > size:
                return False, f"CXRI2 数据不完整：需要 {expected} 字节，实际 {size} 字节"
            meta = data[22:22 + meta_size]
            if b"[Meta]" not in meta or b"HashSeed=" not in meta:
                return False, "CXRI2 缺少 [Meta]/HashSeed"
            return True, f"正常（{size / 1024:.1f} KB）"
        text_head = data[:4096].decode("utf-8-sig", errors="replace")
        if "[Meta]" in text_head and "HashSeed" in text_head:
            return True, f"文本规则（{size / 1024:.1f} KB）"
        return False, f"不是可识别的 rules.int 规则包，大小 {size} 字节"


    def rules_int_backup_path(self, draft: Path) -> Path:
        return draft / "rules.int.valid.bak"

    def save_rules_int_backup(self, draft: Path, reason: str = "") -> bool:
        rules = draft / "rules.int"
        if not rules.exists():
            return False
        ok, issue = self.rules_int_integrity(rules)
        if not ok:
            self.log(f"未备份 rules.int：{issue}")
            return False
        backup = self.rules_int_backup_path(draft)
        try:
            shutil.copy2(rules, backup)
            if reason:
                self.log(f"已备份有效 rules.int（{reason}）：{backup}")
            return True
        except OSError as exc:
            self.log(f"备份 rules.int 失败：{exc}")
            return False

    def restore_rules_int_backup_if_needed(self, draft: Path, action_name: str = "") -> bool:
        rules = draft / "rules.int"
        if rules.exists():
            ok, _issue = self.rules_int_integrity(rules)
            if ok:
                return True
        backup = self.rules_int_backup_path(draft)
        if not backup.exists():
            return False
        backup_ok, backup_issue = self.rules_int_integrity(backup)
        if not backup_ok:
            self.log(f"无法从备份恢复 rules.int：备份无效：{backup_issue}")
            return False
        try:
            shutil.copy2(backup, rules)
            self.log(f"已从有效备份恢复 rules.int{('（' + action_name + '）') if action_name else ''}：{backup}")
            return True
        except OSError as exc:
            self.log(f"恢复 rules.int 备份失败：{exc}")
            return False

    def validate_rules_int_or_warn(self, rules: Path, action_name: str) -> bool:
        ok, issue = self.rules_int_integrity(rules)
        if ok:
            return True
        message = (
            f"{action_name} 已中止。\n\n"
            f"当前 rules.int 无效：{issue}。\n"
            "请重新点击“生成当前游戏扩展集”。如果仍然出现这个问题，请先确认该游戏已经完成动态 XP3 提取/静态 Hash 流程。"
        )
        QMessageBox.information(self, APP_TITLE, message)
        self.last_publisher_test_error = f"rules.int 无效：{issue}"
        self.log(f"{action_name} 中止：rules.int 无效：{issue}，路径：{rules}")
        return False

    def read_int_section(self, path: Path, section: str) -> dict[str, str]:
        return read_int_sections_best_effort(path).get(section.lower(), {})

    def write_manifest_int_sections(self, manifest: Path, updates: dict[str, dict[str, str]]) -> None:
        header = "CXDEC-MANIFEST-INT\t1"
        lines: list[str] = []
        if manifest.exists():
            try:
                lines = manifest.read_text(encoding="utf-8-sig", errors="replace").splitlines()
            except OSError:
                lines = []
        if not lines or not lines[0].startswith("CXDEC-MANIFEST-INT"):
            lines.insert(0, header)

        sections: dict[str, dict[str, str]] = {}
        order: list[str] = []
        current = ""
        for raw_line in lines[1:]:
            line = raw_line.strip()
            if not line:
                continue
            if line.startswith("[") and line.endswith("]"):
                current = line[1:-1].strip()
                if current and current.lower() not in [item.lower() for item in order]:
                    order.append(current)
                    sections[current.lower()] = {}
                continue
            if current and "=" in line:
                key, value = line.split("=", 1)
                sections.setdefault(current.lower(), {})[key.strip()] = value.strip()

        canonical_names = {item.lower(): item for item in order}
        for section, values in updates.items():
            section_key = section.lower()
            if section_key not in sections:
                sections[section_key] = {}
                canonical_names[section_key] = section
                order.append(section)
            for key, value in values.items():
                if value is None:
                    continue
                existing_key = next((item for item in sections[section_key] if item.lower() == key.lower()), key)
                sections[section_key][existing_key] = str(value)

        output = [lines[0] if lines else header]
        for section_name in order:
            section_key = section_name.lower()
            output.append(f"[{canonical_names.get(section_key, section_name)}]")
            for key, value in sections.get(section_key, {}).items():
                output.append(f"{key}={value}")
            output.append("")
        manifest.write_text("\r\n".join(output).rstrip() + "\r\n", encoding="utf-8-sig", newline="")

    def write_int_meta_value(self, path: Path, key: str, value: str) -> bool:
        """Safely update a key inside [Meta] for manifest.int or binary CXRI rules.

        For CXRI2/CXRI3 the [Meta] text length is stored in the binary header.
        Updating bytes without fixing that length corrupts rules.int, so this
        method rebuilds only the header + meta block and leaves pattern/path
        blocks unchanged.
        """
        import re

        def update_meta_block(meta: bytes) -> bytes | None:
            newline = b"\r\n" if b"\r\n" in meta else b"\n"
            meta_match = re.search(br"(?im)^\s*\[Meta\]\s*(?:\r?\n|$)", meta)
            if not meta_match:
                return None
            section_start = meta_match.end()
            next_section = re.search(br"(?im)^\s*\[[^\r\n]+\]\s*(?:\r?\n|$)", meta[section_start:])
            section_end = section_start + next_section.start() if next_section else len(meta)
            section = meta[section_start:section_end]
            key_bytes = key.encode("ascii")
            value_bytes = value.encode("utf-8")
            replacement = key_bytes + b"=" + value_bytes + newline
            key_pattern = re.compile(br"(?im)^\s*" + re.escape(key_bytes) + br"\s*=.*(?:\r?\n|$)")
            if key_pattern.search(section):
                new_section = key_pattern.sub(replacement, section, count=1)
            else:
                new_section = replacement + section
            return meta[:section_start] + new_section + meta[section_end:]

        try:
            data = path.read_bytes()
        except OSError:
            return False

        try:
            if data.startswith(b"CXRI3\0") and len(data) >= 30:
                meta_size = int.from_bytes(data[6:10], "little", signed=False)
                pattern_size = int.from_bytes(data[10:14], "little", signed=False)
                stored_path_block_size = int.from_bytes(data[26:30], "little", signed=False)
                meta_start = 30
                meta_end = meta_start + meta_size
                total_end = meta_end + pattern_size + stored_path_block_size
                if total_end > len(data):
                    return False
                new_meta = update_meta_block(data[meta_start:meta_end])
                if new_meta is None:
                    return False
                header = bytearray(data[:30])
                header[6:10] = len(new_meta).to_bytes(4, "little", signed=False)
                path.write_bytes(bytes(header) + new_meta + data[meta_end:])
                return True
            if data.startswith(b"CXRI2\0") and len(data) >= 22:
                meta_size = int.from_bytes(data[6:10], "little", signed=False)
                pattern_size = int.from_bytes(data[10:14], "little", signed=False)
                path_block_size = int.from_bytes(data[18:22], "little", signed=False)
                meta_start = 22
                meta_end = meta_start + meta_size
                total_end = meta_end + pattern_size + path_block_size
                if total_end > len(data):
                    return False
                new_meta = update_meta_block(data[meta_start:meta_end])
                if new_meta is None:
                    return False
                header = bytearray(data[:22])
                header[6:10] = len(new_meta).to_bytes(4, "little", signed=False)
                path.write_bytes(bytes(header) + new_meta + data[meta_end:])
                return True

            new_data = update_meta_block(data)
            if new_data is None:
                return False
            path.write_bytes(new_data)
            return True
        except OSError:
            return False

    def ensure_draft_hash_seed(self, rules: Path) -> str:
        if rules.suffix.lower() == ".int":
            seed = ExtensionCatalog.read_int_meta(rules).get("hashseed", "")
            if seed:
                return seed
            game_dir = self.game_dir()
            if not game_dir:
                return ""
            seed = self.find_hash_seed_for_game(game_dir)
            if seed and self.write_int_meta_value(rules, "HashSeed", seed):
                self.log(f"已从现有报告回填 HashSeed：{seed}")
                return seed
            return ""

        parser = self.preserve_config_case(configparser.ConfigParser())
        parser.read(rules, encoding="utf-8-sig")
        if not parser.has_section("Meta"):
            parser.add_section("Meta")
        seed = self.config_get_ci(parser, "Meta", "HashSeed")
        if seed:
            parser.set("Meta", "HashSeed", seed)
            with rules.open("w", encoding="utf-8-sig") as handle:
                parser.write(handle)
            return seed
        game_dir = self.game_dir()
        if not game_dir:
            return ""
        seed = self.find_hash_seed_for_game(game_dir)
        if seed:
            parser.set("Meta", "HashSeed", seed)
            with rules.open("w", encoding="utf-8-sig") as handle:
                parser.write(handle)
            self.log(f"已从现有报告回填 HashSeed：{seed}")
        return seed

    def normalize_draft_metadata(
        self,
        manifest: Path,
        rules: Path,
        brand: str,
        game: str,
        author: str = "",
        version: str = "",
        summary: str = "",
        date_text: str = "",
    ) -> None:
        if manifest.suffix.lower() == ".int":
            meta = self.read_int_section(manifest, "Meta")
            updates = {
                "Brand": brand,
                "Game": game,
                "Format": meta.get("format", "manifest.int"),
            }
            if author:
                updates["Contributor"] = author
                updates["Author"] = author
            if version:
                updates["Version"] = version
            if date_text:
                updates["Date"] = normalize_metadata_date(date_text)
            if summary:
                updates["Summary"] = summary
            elif not meta.get("summary"):
                updates["Summary"] = "由制作流程生成的当前游戏扩展集，请测试通过后再安装使用。"
            self.write_manifest_int_sections(manifest, {"Meta": updates})
            # rules.int is a binary rule package; metadata edits must not rewrite it as INI text.
            return
        manifest_parser = self.preserve_config_case(configparser.ConfigParser())
        manifest_parser.read(manifest, encoding="utf-8-sig")
        if not manifest_parser.has_section("Meta"):
            manifest_parser.add_section("Meta")
        manifest_parser.set("Meta", "Brand", brand)
        manifest_parser.set("Meta", "Game", game)
        if author:
            manifest_parser.set("Meta", "Contributor", author)
            manifest_parser.set("Meta", "Author", author)
        if version:
            manifest_parser.set("Meta", "Version", version)
        if date_text:
            manifest_parser.set("Meta", "Date", normalize_metadata_date(date_text))
        if summary:
            manifest_parser.set("Meta", "Summary", summary)
        elif not self.config_get_ci(manifest_parser, "Meta", "Summary"):
            manifest_parser.set("Meta", "Summary", "由制作流程生成的当前游戏扩展集，请测试通过后再安装使用。")
        with manifest.open("w", encoding="utf-8-sig") as handle:
            manifest_parser.write(handle)

        rules_parser = self.preserve_config_case(configparser.ConfigParser())
        rules_parser.read(rules, encoding="utf-8-sig")
        if not rules_parser.has_section("Meta"):
            rules_parser.add_section("Meta")
        rules_parser.set("Meta", "Brand", brand)
        rules_parser.set("Meta", "GameId", game)
        rules_parser.set("Meta", "GameDisplayName", game)
        if author:
            rules_parser.set("Meta", "Contributor", author)
            rules_parser.set("Meta", "Author", author)
        if version:
            rules_parser.set("Meta", "Version", version)
        if date_text:
            rules_parser.set("Meta", "Date", normalize_metadata_date(date_text))
        if summary:
            rules_parser.set("Meta", "Summary", summary)
        existing_seed = self.config_get_ci(rules_parser, "Meta", "HashSeed")
        if existing_seed:
            rules_parser.set("Meta", "HashSeed", existing_seed)
        else:
            rules_parser.set("Meta", "HashSeed", "")
        if not self.config_get_ci(rules_parser, "Meta", "Encoding"):
            rules_parser.set("Meta", "Encoding", "UTF-16LE")
        if rules_parser.has_section("StaticHash"):
            output_dir = self.config_get_ci(rules_parser, "StaticHash", "OutputDir")
            if output_dir:
                rules_parser.set("StaticHash", "OutputDir", output_dir)
        if rules_parser.has_section("Voice"):
            for key in ("VoicePattern", "Prefixes", "PrefixRanges", "MaxNumber", "NumberWidth", "Suffixes"):
                value = self.config_get_ci(rules_parser, "Voice", key)
                if value:
                    rules_parser.set("Voice", key, value)
        with rules.open("w", encoding="utf-8-sig") as handle:
            rules_parser.write(handle)

    def write_publisher_validation_result(
        self,
        manifest: Path,
        draft: Path,
        result: dict[str, str],
    ) -> None:
        if manifest.suffix.lower() == ".int":
            self.write_manifest_int_sections(manifest, {"Validation": result})
        else:
            for path in (manifest,):
                parser = self.preserve_config_case(configparser.ConfigParser())
                parser.read(path, encoding="utf-8-sig")
                if not parser.has_section("Validation"):
                    parser.add_section("Validation")
                for key, value in result.items():
                    parser.set("Validation", key, value)
                with path.open("w", encoding="utf-8-sig") as handle:
                    parser.write(handle)

        result_lines = [
            f"RESTORE={'completed' if result.get('Passed') in ('yes', 'no') else 'failed'}",
            f"PASSED={result.get('Passed', '')}",
            f"TOTAL_FILES={result.get('TotalFiles', '0')}",
            f"RESTORED_FILES={result.get('RestoredFiles', '0')}",
            f"SUCCESS_RATE={result.get('SuccessRate', '-')}",
            f"PACKAGE_BYTES={result.get('PackageBytes', '0')}",
            f"PACKAGE_SIZE_KB={result.get('PackageSizeKB', '0')}",
            f"RESTORE_ERROR={result.get('Error', '')}",
        ]
        (draft / "PublisherTestResult.ini").write_text("\r\n".join(result_lines) + "\r\n", encoding="utf-8-sig", newline="")

        lines = [
            "Cxdec 扩展集测试报告",
            "",
            f"测试结果: {'通过' if result.get('Passed') == 'yes' else '未通过'}",
            f"扩展集大小: {result.get('PackageSizeKB', '0')} KB",
            f"成功还原: {result.get('RestoredFiles', '0')} / {result.get('TotalFiles', '0')}",
            f"成功率: {result.get('SuccessRate', '-')}",
            "",
            '说明: 本报告由"测试当前扩展集"生成，用于记录该扩展集安装前的还原成功率。'
        ]
        (draft / "ValidationReport.txt").write_text("\r\n".join(lines) + "\r\n", encoding="utf-8-sig", newline="")

    def game_path_to_name(self) -> str:
        value = self.game_path.text().strip()
        return Path(value).stem if value else "UnknownGame"

    def next_backup_path(self, target: Path) -> Path:
        index = 1
        while True:
            backup = target.with_name(f"{target.name}.bak{index}")
            if not backup.exists():
                return backup
            index += 1

    def add_numbered_items(self, widget: QListWidget, values: list[str]) -> None:
        """Fill a list with visible numbering while keeping the raw value in UserRole."""
        widget.clear()
        for index, value in enumerate(values, start=1):
            item = QListWidgetItem(f"{index}. {value}")
            item.setData(Qt.UserRole, value)
            widget.addItem(item)

    def current_item_value(self, widget: QListWidget) -> str:
        item = widget.currentItem()
        if not item:
            return ""
        value = item.data(Qt.UserRole)
        return str(value) if value is not None else item.text()

    def reload_extensions(self) -> None:
        entries = self.catalog.load()
        self.populate_user_extension_lists()
        self.add_numbered_items(self.brand_list, self.catalog.brands())
        self.game_list.clear()
        self.log(f"已加载扩展集数量：{len(entries)}")

    def populate_user_extension_lists(self) -> None:
        if not hasattr(self, "user_brand_list"):
            return
        self.catalog.load()
        self.add_numbered_items(self.user_brand_list, self.catalog.brands())
        self.user_game_list.clear()

    def on_user_brand_selected(self, _brand: str) -> None:
        brand = self.current_user_brand()
        self.add_numbered_items(self.user_game_list, [entry.game for entry in self.catalog.games_for_brand(brand)])
        self.refresh_current_page_info()

    def on_user_game_selected(self, _game: str) -> None:
        self.refresh_current_page_info()

    def refresh_current_page_info(self) -> None:
        if not hasattr(self, "stack"):
            return
        index = self.stack.currentIndex()
        if index == 0:
            self.show_user_overview()
        elif index == 1:
            self.show_publisher_status()
        elif index == 2:
            entry = self.current_extension_entry()
            if entry:
                self.show_extension_detail(entry)
            elif self.current_brand:
                self.show_extension_brand_info(self.current_brand, self.catalog.games_for_brand(self.current_brand))
            else:
                self.show_extension_default_info()

    def current_user_brand(self) -> str:
        return self.current_item_value(self.user_brand_list)

    def current_user_extension(self) -> ExtensionEntry | None:
        brand = self.current_user_brand()
        game_item = self.user_game_list.currentItem()
        if not brand or not game_item:
            return None
        game = self.current_item_value(self.user_game_list)
        return next((entry for entry in self.catalog.games_for_brand(brand) if entry.game == game), None)

    def on_brand_selected(self, _brand: str) -> None:
        self._skip_auto_refresh = False
        brand = self.current_item_value(self.brand_list)
        self.current_brand = brand
        entries = self.catalog.games_for_brand(brand)
        self.add_numbered_items(self.game_list, [entry.game for entry in entries])
        self.show_extension_brand_info(brand, entries)

    def on_game_selected(self, game: str) -> None:
        if not game:
            return
        self._skip_auto_refresh = False
        entry = self.current_extension_entry()
        if not entry:
            return
        self.show_extension_detail(entry)

    def current_extension_entry(self) -> ExtensionEntry | None:
        game_item = self.game_list.currentItem()
        if not self.current_brand or not game_item:
            return None
        game = self.current_item_value(self.game_list)
        return next((item for item in self.catalog.games_for_brand(self.current_brand) if item.game == game), None)

    def open_selected_extension(self) -> None:
        entry = self.current_extension_entry()
        if not entry:
            QMessageBox.information(self, APP_TITLE, "请先选择一个扩展集。")
            return
        self.open_path(entry.path)

    def check_online_updates(self) -> None:
        """打开扩展集在线更新对话框。"""
        dialog = OnlineUpdateDialog(self)
        dialog.exec()

    def validate_selected_extension(self) -> None:
        entry = self.current_extension_entry()
        if not entry:
            QMessageBox.information(self, APP_TITLE, "请先选择一个扩展集。")
            return

        issues: list[str] = []
        warnings: list[str] = []
        details: list[str] = []
        manifest_path = entry.path / "manifest.int"
        rules_path = entry.path / "rules.int"

        if not manifest_path.exists():
            warnings.append("缺少 manifest.int，浏览摘要会变少，但不影响核心 Hash。")
        if not rules_path.exists():
            issues.append("缺少 rules.int，使用者流程无法生成静态 Hash。")

        rules_values = self.read_rules_meta(rules_path)
        hash_seed = rules_values.get("hashseed", "")
        voice_pattern = rules_values.get("voicepattern", "") or rules_values.get("pattern", "")
        if rules_path.exists() and not hash_seed:
            issues.append("rules.int 缺少 HashSeed。")

        has_rule_pattern = bool(voice_pattern and rules_values.get("prefixes", "") and (rules_values.get("maxnumber", "") or rules_values.get("prefixranges", "")))
        if not has_rule_pattern:
            issues.append("没有可展开的 VoicePattern 规则。")

        details.append(f"会社: {entry.brand}")
        details.append(f"游戏: {entry.game}")
        details.append(f"路径: {entry.path}")
        details.append(f"HashSeed: {hash_seed or '(未设置)'}")
        details.append(f"扩展集大小: {self.publisher_package_size_kb(entry.path)} KB")
        details.append(f"VoicePattern: {'存在' if has_rule_pattern else '无'}")

        if issues:
            status = "校验结果：不可用，需要修复。"
        elif warnings:
            status = "校验结果：可用，但有提示。"
        else:
            status = "校验结果：可用。"

        lines = ["当前信息", "", status, "", *details]
        if issues:
            lines.extend(["", "必须修复：", *[f"- {item}" for item in issues]])
        if warnings:
            lines.extend(["", "提示：", *[f"- {item}" for item in warnings]])
        self.set_info("\n".join(lines))
        self.log(f"已校验扩展集：{entry.path}")

    def read_rules_meta(self, rules_path: Path) -> dict[str, str]:
        if not rules_path.exists():
            return {}
        if rules_path.suffix.lower() == ".int":
            sections = read_int_sections_best_effort(rules_path)
            values: dict[str, str] = {}
            for section, section_values in sections.items():
                values.update(section_values)
                if section == "pattern:voice":
                    values["voicepattern"] = section_values.get("pattern", "存在") or "存在"
            return values
        values: dict[str, str] = {}
        parser = self.preserve_config_case(configparser.ConfigParser())
        parser.read(rules_path, encoding="utf-8-sig")
        for section in parser.sections():
            for key, value in parser.items(section):
                values[key.lower()] = value.strip()
            if section.lower() == "pattern:voice":
                values["voicepattern"] = self.config_get_ci(parser, section, "Pattern") or "存在"
        return values

    def count_text_lines(self, path: Path) -> int:
        if not path.exists():
            return 0
        count = 0
        with path.open("r", encoding="utf-8-sig", errors="replace") as handle:
            for line in handle:
                text = line.strip()
                if text and not text.startswith(";") and not text.startswith("#"):
                    count += 1
        return count

    def directory_size(self, root: Path) -> int:
        if not root.exists():
            return 0
        total = 0
        for child in root.rglob("*"):
            if child.is_file():
                try:
                    total += child.stat().st_size
                except OSError:
                    continue
        return total

    def directory_size_kb(self, root: Path) -> str:
        return f"{self.directory_size(root) / 1024:.1f}"

    def publisher_package_size(self, root: Path) -> int:
        """正式扩展集口径：只统计运行需要的文件，忽略报告和历史嵌套目录。"""
        if not root.exists():
            return 0
        total = 0
        files = [root / "manifest.int", root / "rules.int"]
        for path in files:
            try:
                if path.is_file():
                    total += path.stat().st_size
            except OSError:
                continue
        return total

    def publisher_package_size_kb(self, root: Path) -> str:
        return f"{self.publisher_package_size(root) / 1024:.1f}"

    def read_text_best_effort(self, path: Path) -> str:
        for encoding in ("utf-8-sig", "utf-16", "utf-16-le", "gb18030"):
            try:
                return path.read_text(encoding=encoding)
            except (UnicodeDecodeError, OSError):
                continue
        return path.read_text(encoding="utf-8", errors="replace")

    def parse_restore_report_counts(self, report: Path) -> dict[str, int]:
        if not report.exists():
            return {"total": 0, "restored": 0}
        content = self.read_text_best_effort(report)
        total = 0
        restored = 0
        for line in content.splitlines():
            line = line.strip()
            if line.startswith("总文件数:") or line.startswith("TOTAL_FILES:"):
                try:
                    total = int(line.split(":", 1)[1].strip())
                except ValueError:
                    pass
            elif line.startswith("成功还原:") or line.startswith("最终还原:") or line.startswith("RESTORED_FILES:"):
                try:
                    raw = line.split(":", 1)[1].strip()
                    # "最终还原"行格式为 "17316 / 18216"，只取前半部分
                    if "/" in raw:
                        raw = raw.split("/", 1)[0].strip()
                    restored = int(raw)
                except ValueError:
                    pass
        return {"total": total, "restored": restored}

    def best_restore_baseline(self, game_dir: Path) -> dict[str, str | int | Path]:
        best: dict[str, str | int | Path] = {"report": "", "total": 0, "restored": 0, "workspace": ""}
        candidates = [
            game_dir / "User" / "1" / "Restored_Extractor_Output" / "RestoreReport.txt",
            game_dir / "User" / "2" / "Restored_Extractor_Output" / "RestoreReport.txt",
            game_dir / "User" / "3" / "Restored_Extractor_Output" / "RestoreReport.txt",
            game_dir / "Restored_Extractor_Output" / "RestoreReport.txt",
            game_dir / "Restored_Extractor_Output" / "RestoreReport.txt" if game_dir.name.startswith("User") else game_dir / "_missing_",
        ]
        for report in candidates:
            counts = self.parse_restore_report_counts(report)
            if counts["restored"] > int(best["restored"]):
                workspace = report.parent.parent
                best = {
                    "report": str(report),
                    "total": counts["total"],
                    "restored": counts["restored"],
                    "workspace": workspace,
                }
        return best

    def workspace_belongs_to_game(self, workspace: Path | None, game_dir: Path) -> bool:
        if not workspace:
            return False
        try:
            workspace.resolve(strict=False).relative_to(game_dir.resolve(strict=False))
            return True
        except (OSError, ValueError):
            return False

    def publisher_source_workspace(self, game_dir: Path) -> Path:
        if (
            self.custom_publisher_source_workspace
            and (self.custom_publisher_source_workspace / "Extractor_Output").exists()
            and self.workspace_belongs_to_game(self.custom_publisher_source_workspace, game_dir)
        ):
            return self.custom_publisher_source_workspace
        if self.custom_publisher_source_workspace and not self.workspace_belongs_to_game(self.custom_publisher_source_workspace, game_dir):
            self.custom_publisher_source_workspace = None
        if (
            self.last_publisher_source_workspace
            and (self.last_publisher_source_workspace / "Extractor_Output").exists()
            and self.workspace_belongs_to_game(self.last_publisher_source_workspace, game_dir)
        ):
            return self.last_publisher_source_workspace
        if self.last_publisher_source_workspace and not self.workspace_belongs_to_game(self.last_publisher_source_workspace, game_dir):
            self.last_publisher_source_workspace = None
        baseline = self.best_restore_baseline(game_dir)
        workspace = baseline.get("workspace", "")
        if isinstance(workspace, Path) and (workspace / "Extractor_Output").exists():
            return workspace
        candidates = [
            game_dir / "User" / "1",
            game_dir / "User" / "2",
            game_dir / "User" / "3",
            game_dir,
        ]
        for workspace_path in candidates:
            if (workspace_path / "Extractor_Output").exists():
                return workspace_path
        return game_dir / "User" / "1"

    def publisher_source_label(self, game_dir: Path | None) -> str:
        if not game_dir:
            return "未选择目标游戏"
        source = self.publisher_source_workspace(game_dir)
        custom = self.custom_publisher_source_workspace and source == self.custom_publisher_source_workspace
        try:
            label = str(source.relative_to(game_dir))
        except ValueError:
            label = source.name or "未知"
        return f"{label}（自定义）" if custom else label

    def restore_rate_text(self, restored: str, total: str) -> str:
        try:
            return f"{(int(restored) / max(int(total), 1) * 100):.2f}%"
        except (TypeError, ValueError):
            return "-"

    def open_integration_doc(self) -> None:
        self.open_path(self.paths.app_dir / "docs" / "original-feature-integration.md")

    def show_help_dialog(self) -> None:
        dialog = HelpDialog(self)
        dialog.exec()

    def show_about_dialog(self) -> None:
        dialog = AboutDialog(self)
        dialog.exec()

    def run_environment_check(self) -> tuple[bool, list[str]]:
        lines: list[str] = []
        ok = True
        lines.append(f"GUI 版本: {app_version_label()}")
        required_files = [
            self.paths.core_cli,
            self.paths.app_dir / "core" / "CxdecExtractor.dll",
            self.paths.app_dir / "core" / "CxdecExtractorUI.dll",
            self.paths.app_dir / "core" / "CxdecStringDumper.dll",
            self.paths.app_dir / "core" / "CxdecKeyDumper.dll",
            self.paths.app_dir / "core" / "CxdecDynamicHashCollector.exe",
        ]
        for path in required_files:
            exists = path.exists()
            ok = ok and exists
            size = path.stat().st_size if exists else 0
            lines.append(f"{'OK' if exists else '缺失'}: {path.name} ({size} bytes)")

        if self.paths.core_cli.exists():
            code, output = self.runner.run(["--version"])
            version_text = output.strip() if code == 0 else f"获取失败，退出码 {code}"
            ok = ok and code == 0
            lines.append(f"CLI 版本: {version_text}")

        extensions_ok = self.paths.extensions_dir.exists()
        ok = ok and extensions_ok
        lines.append(f"{'OK' if extensions_ok else '缺失'}: Extensions 目录")
        entries = self.catalog.load()
        lines.append(f"扩展集数量: {len(entries)}")
        lines.append(f"会社数量: {len(self.catalog.brands())}")
        return ok, lines

    def show_environment_check(self) -> None:
        ok, lines = self.run_environment_check()
        title = "运行环境自检通过" if ok else "运行环境自检发现问题"
        self.set_info("当前信息\n\n" + title + "\n\n" + "\n".join(lines))
        self.log(title)

    def log_environment_check_summary(self) -> None:
        ok, lines = self.run_environment_check()
        status = "通过" if ok else "发现问题"
        extension_line = next((line for line in lines if line.startswith("扩展集数量:")), "扩展集数量: 0")
        self.log(f"运行环境自检{status}，{extension_line}")

    def open_path(self, path: Path) -> None:
        if path.suffix:
            if path.exists():
                subprocess.Popen(["cmd", "/c", "start", "", str(path)], shell=False)
            else:
                QMessageBox.information(self, APP_TITLE, f"文件不存在：{path}")
            return
        path.mkdir(parents=True, exist_ok=True)
        subprocess.Popen(["explorer", str(path)])

    def open_existing_path(self, path: Path) -> None:
        if not path.exists():
            QMessageBox.information(self, APP_TITLE, f"路径不存在：{path}")
            return
        if path.is_file():
            subprocess.Popen(["cmd", "/c", "start", "", str(path)], shell=False)
        else:
            subprocess.Popen(["explorer", str(path)])

    def set_info(self, text: str) -> None:
        self.info.setHtml(self.plain_text_to_info_html(text))

    def set_info_cards(self, title: str, cards: list[tuple[str, list[str] | str]]) -> None:
        # 保存滚动位置
        scroll_pos = self.info.verticalScrollBar().value() if hasattr(self, "info") else 0
        if hasattr(self, "info_group"):
            self.info_group.setTitle(title)
        parts = [
            "<html><head><style>"
            "body{font-family:'Microsoft YaHei UI','Segoe UI';font-size:12px;color:#111;background:#fff;}"
            ".card{border:1px solid #d0d0d0;border-radius:4px;background:#fafafa;margin:0 0 10px 0;padding:9px 10px;}"
            ".title{font-weight:700;margin-bottom:6px;}"
            ".line{margin:2px 0;}"
            ".muted{color:#666;}"
            "</style></head><body>"
        ]
        for heading, body in cards:
            parts.append("<div class='card'>")
            parts.append(f"<div class='title'>{html.escape(heading)}</div>")
            lines = body.splitlines() if isinstance(body, str) else body
            for line in lines:
                if line.strip():
                    parts.append(f"<div class='line'>{html.escape(line)}</div>")
                else:
                    parts.append("<div class='line muted'>&nbsp;</div>")
            parts.append("</div>")
        parts.append("</body></html>")
        self.info.setHtml("".join(parts))
        # 恢复滚动位置（保留用户手动滚动的位置）
        QTimer.singleShot(0, lambda: self.info.verticalScrollBar().setValue(scroll_pos))

    def plain_text_to_info_html(self, text: str) -> str:
        title = "当前信息"
        body = text
        prefix = "当前信息\n\n"
        if text.startswith(prefix):
            body = text[len(prefix):]
        escaped = html.escape(body).replace("\n", "<br>")
        return (
            "<html><head><style>"
            "body{font-family:'Microsoft YaHei UI','Segoe UI';font-size:12px;color:#111;background:#fff;}"
            ".card{border:1px solid #d0d0d0;border-radius:4px;background:#fafafa;margin:0 0 10px 0;padding:9px 10px;}"
            ".title{font-weight:700;margin-bottom:6px;}"
            "</style></head><body>"
            f"<div class='card'><div class='title'>{html.escape(title)}</div>{escaped}</div>"
            "</body></html>"
        )

    def scroll_log_to_bottom(self) -> None:
        """Keep the detail log view pinned to the newest message."""
        bar = self.log_box.verticalScrollBar()
        bar.setValue(bar.maximum())

    def log(self, text: str) -> None:
        self.log_box.appendPlainText(text)
        self.scroll_log_to_bottom()

    def toggle_log_panel(self) -> None:
        visible = not self.log_group.isVisible()
        self.log_group.setVisible(visible)
        self.copy_log_button.setVisible(visible)
        self.clear_log_button.setVisible(visible)
        self.log_toggle_button.setText("隐藏详细日志 ▲" if visible else "显示详细日志 ▼")
        if visible:
            self.scroll_log_to_bottom()

    def expand_log_panel(self) -> None:
        if not self.log_group.isVisible():
            self.toggle_log_panel()

    def copy_log_text(self) -> None:
        QApplication.clipboard().setText(self.log_box.toPlainText())

    def clear_log_text(self) -> None:
        self.log_box.clear()

    def parse_cli_values(self, output: str) -> dict[str, str]:
        values: dict[str, str] = {}
        for line in output.splitlines():
            if ":" not in line:
                continue
            key, value = line.split(":", 1)
            values[key.strip()] = value.strip()
        return values

    def parse_key_value_file(self, path: Path) -> dict[str, str]:
        values: dict[str, str] = {}
        if not path.exists():
            return values
        for line in self.read_text_best_effort(path).splitlines():
            text = line.strip()
            if not text or text.startswith("#") or text.startswith(";"):
                continue
            separator = "=" if "=" in text else (":" if ":" in text else "")
            if not separator:
                continue
            key, value = text.split(separator, 1)
            values[key.strip()] = value.strip()
        return values

    def cli_summary_lines(self, code: int, values: dict[str, str], output: str) -> list[str]:
        if code != 0:
            first_line = output.strip().splitlines()[0] if output.strip() else "无输出"
            return [f"[失败] 核心命令返回错误码 {code}。", f"首行输出：{first_line}"]

        lines = ["[完成] 核心命令执行完成。"]
        if values.get("MODE"):
            lines.append(f"模式：{values['MODE']}")
        if values.get("WORKSPACE"):
            lines.append(f"工作区：{values['WORKSPACE']}")
        if values.get("STATIC_HASH") == "generated":
            lines.append(f"静态 Hash：已生成，输出 {values.get('STATIC_HASH_OUTPUT', '')}")
        if values.get("RESTORE") == "completed":
            total = values.get("TOTAL_FILES", "0")
            restored = values.get("RESTORED_FILES", "0")
            lines.append(f"资源名还原：{restored} / {total}，成功率 {self.restore_rate_text(restored, total)}")
            if values.get("RESTORE_REPORT"):
                lines.append(f"报告：{values['RESTORE_REPORT']}")
        if values.get("DYNAMIC") == "started":
            lines.append(f"动态模块：{values.get('MODULE', '')} 已启动，PID {values.get('PROCESS_ID', '')}")
            if values.get("OUTPUT_ROOT"):
                lines.append(f"输出根目录：{values['OUTPUT_ROOT']}")
        if values.get("PUBLISHER_DRAFT") == "generated":
            lines.append(f"扩展集草稿：已生成，目录 {values.get('DRAFT_DIR', '')}")
        if values.get("NEXT_STEP"):
            lines.append(f"下一步：{values['NEXT_STEP']}")
        return lines

    def show_cli_summary(self, code: int, output: str) -> None:
        values = self.parse_cli_values(output)

        report = values.get("RESTORE_REPORT", "")
        if report:
            self.last_restore_report = Path(report)
            self.last_restored_dir = self.last_restore_report.parent

        draft = values.get("DRAFT_DIR", "")
        if draft:
            self.last_publisher_draft = Path(draft)
        draft_report = values.get("DRAFT_REPORT", "")
        if draft_report:
            self.last_publisher_report = Path(draft_report)

        summary_lines = self.cli_summary_lines(code, values, output)
        self.log("\n".join(summary_lines))

        if code != 0:
            self.set_info_cards("执行结果", [("执行失败", summary_lines), ("原始输出", output or "无输出")])
            self.expand_log_panel()
            return

        self.refresh_current_page_info()

    def open_last_restored_dir(self) -> None:
        target = self.last_restored_dir
        workspace = self.current_user_workspace()
        if workspace:
            target = workspace / "Restored_Extractor_Output"
        if not target or not target.exists():
            QMessageBox.information(self, APP_TITLE, "还原目录还没有生成，请先执行还原步骤。")
            return
        self.last_restored_dir = target
        self.open_existing_path(target)

    def open_last_restore_report(self) -> None:
        workspace = self.current_user_workspace()
        fallback = None
        if workspace:
            fallback = workspace / "Restored_Extractor_Output" / "RestoreReport.txt"
        elif self.last_restored_dir:
            fallback = self.last_restored_dir / "RestoreReport.txt"
        if fallback and fallback.exists():
            self.last_restore_report = fallback
        if not self.last_restore_report:
            QMessageBox.information(self, APP_TITLE, "还没有可打开的还原报告。")
            return
        self.open_existing_path(self.last_restore_report)

    def open_last_publisher_draft(self) -> None:
        draft = self.current_publisher_draft_path()
        if not draft or not draft.exists():
            QMessageBox.information(self, APP_TITLE, "还没有可打开的扩展集草稿目录。")
            return
        self.last_publisher_draft = draft
        self.open_existing_path(draft)

    def open_last_publisher_report(self) -> None:
        if not self.last_publisher_report:
            fallback = self.last_publisher_draft / "PublisherDraftReport.txt" if self.last_publisher_draft else None
            if fallback and fallback.exists():
                self.last_publisher_report = fallback
            else:
                QMessageBox.information(self, APP_TITLE, "还没有可打开的扩展集草稿报告。")
                return
        self.open_existing_path(self.last_publisher_report)

    def open_last_imported_extension(self) -> None:
        if not self.last_imported_extension or not self.last_imported_extension.exists():
            QMessageBox.information(self, APP_TITLE, "还没有可打开的已安装扩展集。请先执行“一键生成并安装最小扩展集”。")
            return
        self.open_existing_path(self.last_imported_extension)

    def _ensure_sample_extension(self) -> None:
        # Do not synthesize .int files here: rules.int is a binary CXRI package.
        # Bundled examples must be produced by the publisher flow and copied in as real packages.
        return


def main() -> int:
    # 普通源码运行时使用脚本目录；PyInstaller 打包后使用 exe 所在目录。
    app_dir = Path(sys.executable).resolve().parent if getattr(sys, "frozen", False) else Path(__file__).resolve().parent
    app = QApplication(sys.argv)
    # frozen 时资源文件在 _runtime/ 中
    if getattr(sys, "frozen", False):
        app_icon = Path(sys._MEIPASS) / "app_icon.ico"
    else:
        app_icon = app_dir / "app_icon.ico"
    if app_icon.exists():
        app.setWindowIcon(QIcon(str(app_icon)))
    window = WorkbenchWindow(WorkbenchPaths(app_dir))
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
