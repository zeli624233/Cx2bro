from __future__ import annotations

import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from tkinter import (
    BOTH,
    END,
    HORIZONTAL,
    LEFT,
    RIGHT,
    VERTICAL,
    X,
    Y,
    Button,
    Entry,
    Frame,
    Label,
    LabelFrame,
    Listbox,
    StringVar,
    Text,
    Tk,
    filedialog,
    messagebox,
    ttk,
)


APP_TITLE = "Cx2bro"


@dataclass
class ExtensionEntry:
    """扩展集的一条游戏记录。"""

    brand: str
    game: str
    path: Path
    hash_seed: str = ""
    summary: str = ""
    has_rules: bool = False
    has_static_input: bool = False
    has_static_output: bool = False


class WorkbenchPaths:
    """集中管理 GUI 会用到的路径，避免到处拼目录。"""

    def __init__(self, app_dir: Path) -> None:
        self.app_dir = app_dir
        self.extensions_dir = app_dir / "Extensions"
        self.logs_dir = app_dir / "Logs"
        self.cache_dir = app_dir / "Cache"
        self.core_cli = app_dir / "core" / "CxdecCoreCLI.exe"

    @staticmethod
    def user_workspace(game_dir: Path, mode: int) -> Path:
        return game_dir / "User" / str(mode)

    @staticmethod
    def publisher_workspace(game_dir: Path) -> Path:
        return game_dir / "Publisher"


class ExtensionCatalog:
    """扫描 Extensions\\会社\\游戏 目录。

    StaticHash_Output 默认运行时生成，不要求扩展集随包携带。
    """

    def __init__(self, root: Path) -> None:
        self.root = root
        self.entries: list[ExtensionEntry] = []

    def load(self) -> list[ExtensionEntry]:
        self.entries.clear()
        if not self.root.exists():
            self.root.mkdir(parents=True, exist_ok=True)
            return self.entries

        for brand_dir in sorted(p for p in self.root.iterdir() if p.is_dir()):
            for game_dir in sorted(p for p in brand_dir.iterdir() if p.is_dir()):
                self.entries.append(self._load_entry(brand_dir.name, game_dir))
        return self.entries

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

        rules_meta = self.read_int_meta(rules_path)
        manifest_meta = self.read_int_meta(manifest_path)
        entry.game = rules_meta.get("gamedisplayname") or manifest_meta.get("game") or entry.game
        entry.hash_seed = rules_meta.get("hashseed", "") or manifest_meta.get("hashseed", "")
        entry.summary = manifest_meta.get("summary", "") or rules_meta.get("summary", "")

        if not entry.summary:
            entry.summary = "已提供 int 规则文件。" if entry.has_rules else "缺少 rules.int。"
        return entry

    @staticmethod
    def read_int_meta(path: Path) -> dict[str, str]:
        values: dict[str, str] = {}
        if not path.exists():
            return values
        current_section = ""
        try:
            for raw_line in path.read_text(encoding="utf-8-sig", errors="replace").splitlines():
                line = raw_line.strip()
                if not line or line.startswith(";") or line.startswith("#"):
                    continue
                if line.startswith("[") and line.endswith("]"):
                    current_section = line[1:-1].strip().lower()
                    continue
                if current_section != "meta" or "=" not in line:
                    continue
                key, value = line.split("=", 1)
                values[key.strip().lower()] = value.strip()
        except OSError:
            pass
        return values


class CoreRunner:
    """Python GUI 到 C++ 核心的边界。

    后续真正接入时，只需要把 mode 和参数转换成 CxdecCoreCLI.exe 的命令行。
    目前如果核心不存在，就只创建工作区并写日志，方便先打磨 GUI。
    """

    def __init__(self, paths: WorkbenchPaths, log) -> None:
        self.paths = paths
        self.log = log

    def run(self, args: list[str]) -> int:
        if not self.paths.core_cli.exists():
            self.log(f"核心 CLI 未接入，跳过执行：{' '.join(args)}")
            return 0

        command = [str(self.paths.core_cli), *args]
        self.log("执行：" + " ".join(command))
        completed = subprocess.run(command, text=True, capture_output=True)
        if completed.stdout:
            self.log(completed.stdout.rstrip())
        if completed.stderr:
            self.log(completed.stderr.rstrip())
        return completed.returncode


class WorkbenchApp:
    """主窗口。界面风格参照传统工具软件：顶部功能切换，左侧操作，右侧信息。"""

    def __init__(self, root: Tk, paths: WorkbenchPaths) -> None:
        self.root = root
        self.paths = paths
        self.catalog = ExtensionCatalog(paths.extensions_dir)
        self.runner = CoreRunner(paths, self.log)
        self.game_path = StringVar()
        self.current_brand = ""

        self.root.title(APP_TITLE)
        self.root.geometry("1180x760")
        self.root.minsize(980, 680)

        self._configure_style()
        self._build_shell()
        self.show_user_page()

    def _configure_style(self) -> None:
        style = ttk.Style()
        if "vista" in style.theme_names():
            style.theme_use("vista")
        style.configure("TButton", padding=(10, 4))
        style.configure("TLabelframe", padding=8)

    def _build_shell(self) -> None:
        top = Frame(self.root, padx=12, pady=10)
        top.pack(fill=X)

        Button(top, text="使用者", width=12, command=self.show_user_page).pack(side=LEFT, padx=(0, 8))
        Button(top, text="发布者", width=12, command=self.show_publisher_page).pack(side=LEFT, padx=(0, 8))
        Button(top, text="拓展性", width=12, command=self.show_extension_page).pack(side=LEFT, padx=(0, 8))

        body = Frame(self.root, padx=12)
        body.pack(fill=BOTH, expand=True)

        self.left = Frame(body, width=430)
        self.left.pack(side=LEFT, fill=Y)
        self.left.pack_propagate(False)

        self.right = LabelFrame(body, text="当前信息")
        self.right.pack(side=RIGHT, fill=BOTH, expand=True, padx=(12, 0))
        self.info = Text(self.right, wrap="word", height=12)
        self.info.pack(fill=BOTH, expand=True, padx=6, pady=6)

        log_frame = LabelFrame(self.root, text="日志", padx=8, pady=6)
        log_frame.pack(fill=X, padx=12, pady=(8, 12))
        self.log_box = Text(log_frame, height=6, wrap="word")
        self.log_box.pack(fill=X)

    def _clear_left(self) -> None:
        for child in self.left.winfo_children():
            child.destroy()

    def _set_info(self, text: str) -> None:
        self.info.delete("1.0", END)
        self.info.insert(END, text)

    def log(self, text: str) -> None:
        if not hasattr(self, "log_box"):
            return
        self.log_box.insert(END, text + "\n")
        self.log_box.see(END)

    def _add_game_picker(self, parent: Frame) -> None:
        row = Frame(parent)
        row.pack(fill=X, pady=(4, 8))
        Label(row, text="目标游戏", width=10, anchor="w").pack(side=LEFT)
        Entry(row, textvariable=self.game_path).pack(side=LEFT, fill=X, expand=True, padx=(0, 8))
        Button(row, text="选择", command=self.select_game).pack(side=RIGHT)

    def select_game(self) -> None:
        path = filedialog.askopenfilename(
            title="选择游戏主程序",
            filetypes=[("Executable", "*.exe"), ("All files", "*.*")],
        )
        if not path:
            return
        self.game_path.set(path)
        self.log(f"已选择游戏：{path}")

    def game_dir(self) -> Path | None:
        value = self.game_path.get().strip()
        if not value:
            messagebox.showinfo(APP_TITLE, "请先选择游戏主程序。")
            return None
        return Path(value).parent

    def prepare_user_workspace(self, mode: int) -> None:
        game_dir = self.game_dir()
        if not game_dir:
            return
        root = WorkbenchPaths.user_workspace(game_dir, mode)
        for name in ("Extractor_Output", "StaticHash_Output", "Restored_Extractor_Output"):
            (root / name).mkdir(parents=True, exist_ok=True)
        self.log(f"已准备使用者工作区：{root}")
        self.runner.run(["--mode", f"user{mode}", "--game", self.game_path.get()])

    def show_user_page(self) -> None:
        self._clear_left()
        group = LabelFrame(self.left, text="使用者功能", padx=8, pady=8)
        group.pack(fill=X)
        self._add_game_picker(group)
        Button(group, text="该作扩展集提取", command=lambda: self.prepare_user_workspace(1)).pack(fill=X, pady=3)
        Button(group, text="该会社集合撞新作", command=lambda: self.prepare_user_workspace(2)).pack(fill=X, pady=3)
        Button(group, text="传统动态模式", command=lambda: self.prepare_user_workspace(3)).pack(fill=X, pady=3)

        note = LabelFrame(self.left, text="流程说明", padx=8, pady=8)
        note.pack(fill=X, pady=(12, 0))
        Label(note, text="功能1/2：扩展集生成静态 Hash，再结合动态 XP3 输出还原。", anchor="w", justify=LEFT).pack(fill=X)
        Label(note, text="功能3：加载动态 Hash 模块，进游戏跑资源路径后还原。", anchor="w", justify=LEFT).pack(fill=X)
        self._set_info("当前信息\n\n使用者功能待执行。")

    def show_publisher_page(self) -> None:
        self._clear_left()
        group = LabelFrame(self.left, text="发布者功能", padx=8, pady=8)
        group.pack(fill=X)
        self._add_game_picker(group)
        Button(group, text="制作该游戏扩展集", command=self.prepare_publisher_workspace).pack(fill=X, pady=3)
        Button(group, text="导入/搭载扩展集", command=self.import_extension_placeholder).pack(fill=X, pady=3)
        Button(group, text="打开扩展集目录", command=lambda: self.open_directory(self.paths.extensions_dir)).pack(fill=X, pady=3)

        note = LabelFrame(self.left, text="制作说明", padx=8, pady=8)
        note.pack(fill=X, pady=(12, 0))
        Label(note, text="发布者输出进入 Publisher\\ExtensionDraft。", anchor="w").pack(fill=X)
        Label(note, text="整理完成后可导入到 Extensions\\会社\\游戏。", anchor="w").pack(fill=X)
        self._set_info("当前信息\n\n发布者功能待执行。")

    def prepare_publisher_workspace(self) -> None:
        game_dir = self.game_dir()
        if not game_dir:
            return
        root = WorkbenchPaths.publisher_workspace(game_dir) / "ExtensionDraft"
        root.mkdir(parents=True, exist_ok=True)
        self.log(f"已准备发布者工作区：{root}")
        self.runner.run(["--mode", "publisher-make", "--game", self.game_path.get()])

    def import_extension_placeholder(self) -> None:
        messagebox.showinfo(APP_TITLE, "后续会把发布者扩展集复制到 Extensions 目录。")

    def show_extension_page(self) -> None:
        self._clear_left()
        group = LabelFrame(self.left, text="扩展集浏览", padx=8, pady=8)
        group.pack(fill=BOTH, expand=True)

        toolbar = Frame(group)
        toolbar.pack(fill=X, pady=(0, 8))
        Button(toolbar, text="刷新扩展集", command=self.reload_extensions).pack(side=LEFT, padx=(0, 6))
        Button(toolbar, text="打开扩展集目录", command=lambda: self.open_directory(self.paths.extensions_dir)).pack(side=LEFT, padx=(0, 6))
        Button(toolbar, text="原版功能集成说明", command=self.open_integration_doc).pack(side=LEFT)

        lists = Frame(group)
        lists.pack(fill=BOTH, expand=True)
        brand_frame = Frame(lists)
        brand_frame.pack(side=LEFT, fill=BOTH, expand=True, padx=(0, 6))
        game_frame = Frame(lists)
        game_frame.pack(side=RIGHT, fill=BOTH, expand=True)

        Label(brand_frame, text="会社", anchor="w").pack(fill=X)
        self.brand_list = Listbox(brand_frame, exportselection=False)
        self.brand_list.pack(fill=BOTH, expand=True)
        self.brand_list.bind("<<ListboxSelect>>", self.on_brand_selected)

        Label(game_frame, text="游戏", anchor="w").pack(fill=X)
        self.game_list = Listbox(game_frame, exportselection=False)
        self.game_list.pack(fill=BOTH, expand=True)
        self.game_list.bind("<<ListboxSelect>>", self.on_game_selected)

        self.reload_extensions()

    def reload_extensions(self) -> None:
        entries = self.catalog.load()
        if not hasattr(self, "brand_list"):
            return
        self.brand_list.delete(0, END)
        self.game_list.delete(0, END)
        for brand in self.catalog.brands():
            self.brand_list.insert(END, brand)
        self._set_info("当前信息\n\n请在左侧选择会社和游戏。")
        self.log(f"已加载扩展集数量：{len(entries)}")

    def on_brand_selected(self, _event) -> None:
        selection = self.brand_list.curselection()
        if not selection:
            return
        self.current_brand = self.brand_list.get(selection[0])
        self.game_list.delete(0, END)
        for entry in self.catalog.games_for_brand(self.current_brand):
            self.game_list.insert(END, entry.game)
        self._set_info("当前信息\n\n请选择游戏。")

    def on_game_selected(self, _event) -> None:
        selection = self.game_list.curselection()
        if not selection:
            return
        game = self.game_list.get(selection[0])
        entry = next((item for item in self.catalog.games_for_brand(self.current_brand) if item.game == game), None)
        if not entry:
            return
        self._set_info(
            "当前信息\n\n"
            f"会社: {entry.brand}\n"
            f"游戏: {entry.game}\n"
            f"HashSeed: {entry.hash_seed or '(未设置)'}\n"
            f"rules.int: {'存在' if entry.has_rules else '缺少'}\n"
            f"StaticHash_Output: {'存在' if entry.has_static_output else '运行时生成'}\n"
            f"路径: {entry.path}\n\n"
            f"{entry.summary}"
        )

    def open_integration_doc(self) -> None:
        self.open_directory(self.paths.app_dir / "docs" / "original-feature-integration.md")

    @staticmethod
    def open_directory(path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        if path.suffix:
            if path.exists():
                subprocess.Popen(["cmd", "/c", "start", "", str(path)], shell=False)
            return
        path.mkdir(parents=True, exist_ok=True)
        subprocess.Popen(["explorer", str(path)])


def ensure_sample_extension(paths: WorkbenchPaths) -> None:
    """Do not synthesize extension packages; rules.int must be a real CXRI package."""
    paths.extensions_dir.mkdir(parents=True, exist_ok=True)


def main() -> int:
    app_dir = Path(__file__).resolve().parent
    paths = WorkbenchPaths(app_dir)
    ensure_sample_extension(paths)

    root = Tk()
    WorkbenchApp(root, paths)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
