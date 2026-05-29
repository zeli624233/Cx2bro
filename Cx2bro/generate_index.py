# 扫描 Extensions/ 目录生成 EXTENSIONS_INDEX.txt
# 用法：把本脚本放在 Cx2bro-Extensions 仓库根目录运行
# 自动读取 manifest.int 中的字段

from pathlib import Path
import datetime

ext_dir = Path("Extensions")
index_path = Path("EXTENSIONS_INDEX.txt")

if not ext_dir.is_dir():
    print("请在 Cx2bro-Extensions 仓库根目录运行本脚本。")
    raise SystemExit(1)


def read_meta_value(manifest: Path, section: str, *keys: str) -> str:
    """读取 manifest.int 中指定段(section)的 key 值。"""
    if not manifest.exists():
        return ""
    in_section = False
    for line in manifest.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        text = line.strip()
        if not text or text.startswith(";") or text.startswith("#"):
            continue
        if text.startswith("[") and text.endswith("]"):
            in_section = text[1:-1].strip().lower() == section.lower()
            continue
        if in_section:
            for key in keys:
                if text.startswith(key + "="):
                    return text.split("=", 1)[1].strip()
    return ""


lines = ["# Cx2bro 扩展集索引", "# 每行格式：入库编号|会社|作品|发售日|入库日|解包率|体积|贡献者|备注"]
index = 1

for brand_dir in sorted(ext_dir.iterdir()):
    if not brand_dir.is_dir():
        continue
    for game_dir in sorted(brand_dir.iterdir()):
        if not game_dir.is_dir():
            continue
        manifest = game_dir / "manifest.int"
        rules = game_dir / "rules.int"

        # 读取元数据
        release_date = read_meta_value(manifest, "Meta", "ReleaseDate") or "?"
        added_date = read_meta_value(manifest, "Meta", "Date") or datetime.date.today().strftime("%Y-%m-%d")
        rate = read_meta_value(manifest, "Validation", "SuccessRate", "SuccessRate") or "?"
        contributor = read_meta_value(manifest, "Meta", "Contributor", "Author") or "?"
        note = read_meta_value(manifest, "Meta", "Summary") or ""

        # 体积：manifest.int + rules.int
        total_kb = 0
        for f in (manifest, rules):
            if f.exists():
                total_kb += f.stat().st_size
        size_kb = f"{total_kb / 1024:.1f}KB"

        line = f"{index:02d}|{brand_dir.name}|{game_dir.name}|{release_date}|{added_date}|{rate}|{size_kb}|{contributor}"
        if note:
            line += f"|{note}"
        lines.append(line)
        index += 1

index_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
print(f"已生成 {index_path}，共 {index - 1} 个扩展集。")
