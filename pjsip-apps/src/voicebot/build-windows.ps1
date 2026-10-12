param(
    [Parameter(Mandatory = $true)][string]$QtPath,
    [string]$BuildDirectory = "",
    [string]$InstallDirectory = ""
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "../../..")).Path
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $root "out/windows/build" }
if (-not $InstallDirectory) { $InstallDirectory = Join-Path $root "out/windows" }
$QtPath = (Resolve-Path $QtPath).Path
if (-not (Test-Path (Join-Path $QtPath "lib/cmake/Qt6/Qt6Config.cmake"))) {
    throw "QtPath must point to a Qt 6 MSVC x64 SDK containing Widgets and WebSockets."
}
$locator = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio/Installer/vswhere.exe"
if (-not (Test-Path $locator)) { throw "Install Visual Studio C++ Build Tools and Windows SDK first." }
$vs = (& $locator -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json | Select-Object -First 1)
if (-not $vs) { throw "Visual Studio C++ x64 tools were not found." }
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmake) { $cmake = $cmake.Source } else {
    $cmake = Join-Path $vs.installationPath "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
}
if (-not (Test-Path $cmake)) { throw "Install CMake 3.28 or newer (VS 2026 requires CMake with that generator)." }
$major = ([version]$vs.installationVersion).Major
$generator = switch ($major) {
    17 { "Visual Studio 17 2022" }
    18 { "Visual Studio 18 2026" }
    default { throw "Use Visual Studio 2022 or 2026 C++ tools." }
}
& $cmake -S $PSScriptRoot -B $BuildDirectory -G $generator -A x64 "-DCMAKE_PREFIX_PATH=$QtPath"
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }
& $cmake --build $BuildDirectory --config Release --target voicebot --parallel 3
if ($LASTEXITCODE -ne 0) { throw "Build failed." }
& $cmake --install $BuildDirectory --config Release --prefix $InstallDirectory
if ($LASTEXITCODE -ne 0) { throw "Deployment failed." }
$windeployqt = Join-Path $QtPath "bin/windeployqt.exe"
if (-not (Test-Path $windeployqt)) { throw "windeployqt not found under QtPath/bin." }
& $windeployqt --release --no-translations (Join-Path $InstallDirectory "bin/voicebot.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed." }
Write-Output "Application: $InstallDirectory/bin/voicebot.exe"
