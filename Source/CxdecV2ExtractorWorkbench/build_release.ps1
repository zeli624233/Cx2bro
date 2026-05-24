$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$ReleaseRoot = Join-Path $Root "release"
$AppName = "CxdecV2ExtractorWorkbench"
$AppDir = Join-Path $ReleaseRoot $AppName
# 自动读取当前 Python 环境中的 PySide6 路径，避免写死到某台电脑的目录。
$PySidePath = $null
try {
    $PySidePath = (& python -c "import pathlib, PySide6; print(pathlib.Path(PySide6.__file__).resolve().parent)").Trim()
} catch {
    $PySidePath = $null
}

if (Test-Path -LiteralPath $ReleaseRoot) {
    Remove-Item -LiteralPath $ReleaseRoot -Recurse -Force
}

$PyInstallerArgs = @(
    "--noconfirm",
    "--clean",
    "--onedir",
    "--windowed",
    "--name", $AppName,
    "--icon", (Join-Path $Root "app_icon.ico"),
    "--distpath", $ReleaseRoot,
    "--workpath", (Join-Path $Root "build\pyinstaller"),
    "--specpath", (Join-Path $Root "build"),
    "--contents-directory", "_runtime"
)

if ($PySidePath -and (Test-Path -LiteralPath $PySidePath)) {
    $PyInstallerArgs += @("--paths", $PySidePath)
}

$PyInstallerArgs += (Join-Path $Root "cxdec_gui_pyside.py")
python -m PyInstaller @PyInstallerArgs

Copy-Item -LiteralPath (Join-Path $Root "core") -Destination (Join-Path $AppDir "core") -Recurse -Force
Copy-Item -LiteralPath (Join-Path $Root "Extensions") -Destination (Join-Path $AppDir "Extensions") -Recurse -Force
Copy-Item -LiteralPath (Join-Path $Root "README.md") -Destination (Join-Path $AppDir "README.md") -Force
Copy-Item -LiteralPath (Join-Path $Root "RELEASE_LAYOUT.md") -Destination (Join-Path $AppDir "RELEASE_LAYOUT.md") -Force
foreach ($NoticeFile in @("THIRD_PARTY_NOTICES.md", "LICENSE_NOTICE.md")) {
    $NoticePath = Join-Path $Root $NoticeFile
    if (Test-Path -LiteralPath $NoticePath) {
        Copy-Item -LiteralPath $NoticePath -Destination (Join-Path $AppDir $NoticeFile) -Force
    }
}
Copy-Item -LiteralPath (Join-Path $Root "app_icon.ico") -Destination (Join-Path $AppDir "app_icon.ico") -Force
if (Test-Path -LiteralPath (Join-Path $Root "about_avatar.jpg")) {
    Copy-Item -LiteralPath (Join-Path $Root "about_avatar.jpg") -Destination (Join-Path $AppDir "about_avatar.jpg") -Force
}

$RemoveTargets = @(
    (Join-Path $AppDir "__pycache__"),
    (Join-Path $AppDir "Cache")
)
foreach ($Target in $RemoveTargets) {
    if (Test-Path -LiteralPath $Target) {
        Remove-Item -LiteralPath $Target -Recurse -Force
    }
}

Write-Host "Release created:"
Write-Host $AppDir
