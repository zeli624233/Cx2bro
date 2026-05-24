$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$ReleaseRoot = Join-Path $Root "release"
$AppName = "Cx2bro"
$AppDir = Join-Path $ReleaseRoot $AppName
$BuildDir = Join-Path $Root "build"

# 强制清理所有缓存，确保每次都是完全干净的编译
foreach ($_dir in @($ReleaseRoot, $BuildDir)) {
    if (Test-Path -LiteralPath $_dir) {
        Remove-Item -LiteralPath $_dir -Recurse -Force
        Write-Host "已清理: $_dir"
    }
}

# 检查源码文件存在
$SourcePy = Join-Path $Root "cxdec_gui_pyside.py"
if (-not (Test-Path -LiteralPath $SourcePy)) {
    Write-Error "找不到主源码: $SourcePy"
    exit 1
}
Write-Host "源码: $SourcePy"
Write-Host "源码修改时间: $((Get-Item $SourcePy).LastWriteTime)"
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

# 后处理：复制额外文件 + 清理

Copy-Item -LiteralPath (Join-Path $Root "core") -Destination (Join-Path $AppDir "core") -Recurse -Force
Copy-Item -LiteralPath (Join-Path $Root "Extensions") -Destination (Join-Path $AppDir "Extensions") -Recurse -Force
foreach ($_doc in @("README.md", "RELEASE_LAYOUT.md", "THIRD_PARTY_NOTICES.md", "LICENSE_NOTICE.md")) {
    $_p = Join-Path $Root $_doc
    if (Test-Path -LiteralPath $_p) { Copy-Item -LiteralPath $_p -Destination (Join-Path $AppDir $_doc) -Force }
}
# 将 ico 和 jpg 复制到 _runtime/ 下，避免 exe 旁边有散文件
$runtimeDir = Join-Path $AppDir "_runtime"
if (Test-Path -LiteralPath (Join-Path $Root "app_icon.ico")) {
    Copy-Item -LiteralPath (Join-Path $Root "app_icon.ico") -Destination (Join-Path $runtimeDir "app_icon.ico") -Force
}
if (Test-Path -LiteralPath (Join-Path $Root "about_avatar.jpg")) {
    Copy-Item -LiteralPath (Join-Path $Root "about_avatar.jpg") -Destination (Join-Path $runtimeDir "about_avatar.jpg") -Force
}
# 清理 __pycache__ 和 Cache
foreach ($RmTarget in @((Join-Path $AppDir "__pycache__"), (Join-Path $AppDir "Cache"))) {
    if (Test-Path -LiteralPath $RmTarget) { Remove-Item -LiteralPath $RmTarget -Recurse -Force }
}

# 清理 _runtime 中的 .pdb 调试符号
if (Test-Path -LiteralPath $runtimeDir) {
    Get-ChildItem -LiteralPath $runtimeDir -Recurse -Filter "*.pdb" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
}

# 删除未使用的 Qt6 DLL + 翻译文件
$pysideDir = Join-Path $runtimeDir "PySide6"
if (Test-Path -LiteralPath $pysideDir) {
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
        Get-ChildItem -LiteralPath $pysideDir -Filter $Pattern -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
    }
    $transDir = Join-Path $pysideDir "translations"
    if (Test-Path -LiteralPath $transDir) { Remove-Item -LiteralPath $transDir -Recurse -Force }
}
Write-Host "Release created:"
Write-Host $AppDir

# 部署到 编译版本 目录（删除旧的再复制新的）
$DeployDir = Join-Path $Root "..\编译版本\Cx2bro"
$DeployRoot = Join-Path $Root "..\编译版本"
if (Test-Path -LiteralPath $DeployDir) {
    Remove-Item -LiteralPath $DeployDir -Recurse -Force
    Write-Host "已清理旧部署: $DeployDir"
}
if (-not (Test-Path -LiteralPath $DeployRoot)) {
    New-Item -ItemType Directory -Path $DeployRoot -Force | Out-Null
}
Copy-Item -LiteralPath $AppDir -Destination $DeployDir -Recurse -Force
Write-Host "已部署到: $DeployDir"

# 验证：exe 的时间戳应该 >= 源码时间戳
$ExePath = Join-Path $AppDir "$AppName.exe"
$DeployExe = Join-Path $DeployDir "$AppName.exe"
$SourceTime = (Get-Item $SourcePy).LastWriteTime
$ExeTime = (Get-Item $ExePath).LastWriteTime
if ($ExeTime -lt $SourceTime) {
    Write-Warning "exe 时间戳 ($ExeTime) 早于源码 ($SourceTime)，编译可能不是最新的！"
} else {
    Write-Host "时间戳验证: exe ($ExeTime) >= 源码 ($SourceTime) ✅"
}
Write-Host "构建完成，体积: $((Get-Item $DeployExe).Length / 1KB) KB"

