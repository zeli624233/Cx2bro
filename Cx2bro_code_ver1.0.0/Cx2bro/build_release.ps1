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

# 排除未使用的 PySide6 模块（省掉 QtWebEngine/QtQuick/QtNetwork 等大块头）
$ExcludeModules = @("PySide6.QtWebEngineWidgets","PySide6.QtWebEngineCore","PySide6.QtWebEngineQuick","PySide6.QtWebChannel","PySide6.QtWebSockets","PySide6.QtWebView","PySide6.QtQuick","PySide6.QtQuick3D","PySide6.QtQuickDialogs2","PySide6.QtQuickControls2","PySide6.QtQuickTemplates2","PySide6.QtQml","PySide6.QtQmlCompiler","PySide6.QtQmlModels","PySide6.QtNetwork","PySide6.QtNetworkAuth","PySide6.QtMultimedia","PySide6.QtMultimediaWidgets","PySide6.QtDesigner","PySide6.QtDesignerComponents","PySide6.QtPdf","PySide6.QtPdfWidgets","PySide6.QtSvg","PySide6.QtSvgWidgets","PySide6.QtSql","PySide6.QtXml","PySide6.QtTest","PySide6.QtBluetooth","PySide6.QtNfc","PySide6.QtPositioning","PySide6.QtSensors","PySide6.QtSerialPort","PySide6.QtSerialBus","PySide6.QtCharts","PySide6.QtDataVisualization","PySide6.QtPrintSupport","PySide6.QtTextToSpeech","PySide6.QtLocation","PySide6.QtGraphs","PySide6.QtHttpServer","PySide6.QtScxml","PySide6.QtStateMachine","PySide6.QtRemoteObjects","PySide6.QtConcurrent","PySide6.QtHelp","PySide6.QtUiTools","PySide6.Qt3DCore","PySide6.Qt3DRender","PySide6.Qt3DInput","PySide6.Qt3DLogic","PySide6.Qt3DExtras","PySide6.Qt3DAnimation","PySide6.QtVirtualKeyboard","PySide6.QtLottie","PySide6.QtSpatialAudio","PySide6.QtShaderTools")
foreach ($Mod in $ExcludeModules) {
    $PyInstallerArgs += "--exclude-module"
    $PyInstallerArgs += $Mod
}

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

# 清理 _runtime 中的 .pdb 调试符号文件（PyInstaller 有时会连带打包）
$RuntimeDir = Join-Path $AppDir "_runtime"
if (Test-Path -LiteralPath $RuntimeDir) {
    Get-ChildItem -LiteralPath $RuntimeDir -Recurse -Filter "*.pdb" | Remove-Item -Force
}

# 删除未使用的 Qt6 DLL（排除模块不是总能生效，直接删 DLL 最彻底）
$PySideDir = Join-Path $RuntimeDir "PySide6"
if (Test-Path -LiteralPath $PySideDir) {
    $UnusedDlls = @(
        "Qt6Network*", "Qt6Pdf*", "Qt6Qml*", "Qt6Quick*",
        "Qt6VirtualKeyboard*", "Qt6Svg*",
        "Qt6OpenGL*", "Qt6Designer*", "Qt6Charts*",
        "Qt6DataVisualization*", "Qt6Bluetooth*", "Qt6Nfc*",
        "Qt6WebEngine*", "Qt6WebChannel*", "Qt6WebSockets*",
        "Qt6Positioning*", "Qt6Location*", "Qt6Multimedia*",
        "Qt6SerialPort*", "Qt6SerialBus*", "Qt3D*",
        "Qt6Scxml*", "Qt6StateMachine*", "Qt6Concurrent*",
        "Qt6Help*", "Qt6Test*", "Qt6Sql*", "Qt6Xml*",
        "Qt6TextToSpeech*", "Qt6SpatialAudio*", "Qt6HttpServer*",
        "Qt6RemoteObjects*", "Qt6UiTools*", "Qt6ShaderTools*",
        "Qt6Graphs*", "Qt6Lottie*", "Qt6Labs*"
    )
    foreach ($Pattern in $UnusedDlls) {
        Get-ChildItem -LiteralPath $PySideDir -Filter $Pattern -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
    }

    # 删除翻译文件（6MB+，应用用不到 Qt 的界面翻译）
    $TranslationsDir = Join-Path $PySideDir "translations"
    if (Test-Path -LiteralPath $TranslationsDir) {
        Remove-Item -LiteralPath $TranslationsDir -Recurse -Force
    }
}
Write-Host "Release created:"
Write-Host $AppDir
