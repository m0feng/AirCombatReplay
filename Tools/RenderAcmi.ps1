[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$AcmiPath,

    [ValidateRange(1, 120)]
    [int]$Fps = 30,

    [ValidateRange(0.25, 4.0)]
    [double]$PlaybackRate = 0.5,

    [ValidateRange(0, 3600)]
    [double]$ReplayLimitSeconds = 0,

    [ValidateRange(320, 7680)]
    [int]$Width = 2560,

    [ValidateRange(240, 4320)]
    [int]$Height = 1440,

    [ValidateRange(0.001, 10.0)]
    [double]$TargetDt = 0.1,

    [ValidateRange(0, 101)]
    [int]$SmoothingWindow = 0,

    [ValidateRange(0, 120)]
    [double]$WarmupSeconds = 8,

    [ValidateRange(10, 600)]
    [double]$MaxWarmupSeconds = 90,

    [string]$PythonPath = "",

    [string]$EngineRoot = "F:\epic\games\UE_5.6",

    [string]$OutputDirectory = "",

    [string]$FfmpegPath = "ffmpeg",

    [switch]$SkipBuild,

    [switch]$KeepCsv,

    [switch]$ShowWindow,

    [switch]$KeepProxy,

    [switch]$PngSequence
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$converter = Join-Path $projectRoot "acmi_to_npy.py"
$renderScript = Join-Path $PSScriptRoot "RenderRound.ps1"

if (-not (Test-Path -LiteralPath $AcmiPath -PathType Leaf)) {
    throw "ACMI file not found: $AcmiPath"
}
if (-not (Test-Path -LiteralPath $converter -PathType Leaf)) {
    throw "ACMI converter not found: $converter"
}
if (-not (Test-Path -LiteralPath $renderScript -PathType Leaf)) {
    throw "Round renderer not found: $renderScript"
}
$editorExecutable = Join-Path $EngineRoot "Engine\Binaries\Win64\UnrealEditor.exe"
if (-not (Test-Path -LiteralPath $editorExecutable -PathType Leaf)) {
    throw "Unreal Editor not found. Pass the UE 5.6 directory with -EngineRoot."
}
if (-not $PngSequence) {
    $ffmpegCommand = Get-Command $FfmpegPath -ErrorAction SilentlyContinue
    if (-not $ffmpegCommand) {
        throw "ffmpeg was not found. Pass ffmpeg.exe with -FfmpegPath."
    }
}

$AcmiPath = [System.IO.Path]::GetFullPath($AcmiPath)
$pythonExecutable = ""
if ([string]::IsNullOrWhiteSpace($PythonPath)) {
    $bundledPython = Join-Path $EngineRoot "Engine\Binaries\ThirdParty\Python3\Win64\python.exe"
    if (Test-Path -LiteralPath $bundledPython -PathType Leaf) {
        $pythonExecutable = $bundledPython
    }
    else {
        $pythonCommand = Get-Command "py.exe" -ErrorAction SilentlyContinue
        if ($pythonCommand) {
            $pythonExecutable = $pythonCommand.Source
        }
    }
}
elseif (Test-Path -LiteralPath $PythonPath -PathType Leaf) {
    $pythonExecutable = [System.IO.Path]::GetFullPath($PythonPath)
}
else {
    $pythonCommand = Get-Command $PythonPath -ErrorAction SilentlyContinue
    if ($pythonCommand) {
        $pythonExecutable = $pythonCommand.Source
    }
}
if ([string]::IsNullOrWhiteSpace($pythonExecutable)) {
    throw "Python was not found. Pass python.exe with -PythonPath."
}

$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $sourceName = [System.IO.Path]::GetFileNameWithoutExtension($AcmiPath)
    if ($sourceName.EndsWith(".txt", [System.StringComparison]::OrdinalIgnoreCase)) {
        $sourceName = [System.IO.Path]::GetFileNameWithoutExtension($sourceName)
    }
    $safeSourceName = $sourceName -replace '[<>:"/\\|?*\x00-\x1F]', '_'
    if ([string]::IsNullOrWhiteSpace($safeSourceName)) {
        $safeSourceName = "Round"
    }
    $OutputDirectory = Join-Path $projectRoot "Saved\RoundRenders\${safeSourceName}_$stamp"
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
$csvDirectory = Join-Path $OutputDirectory "ReplayCsv_$stamp"
New-Item -ItemType Directory -Path $csvDirectory -Force | Out-Null

Write-Host "Converting ACMI to replay CSV..." -ForegroundColor Cyan
& $pythonExecutable $converter `
    --path $AcmiPath `
    --output-dir $csvDirectory `
    --target-dt $TargetDt `
    --smoothing-window $SmoothingWindow
if ($LASTEXITCODE -ne 0) {
    throw "ACMI conversion failed with exit code $LASTEXITCODE."
}

$manifestPath = Join-Path $csvDirectory "Match_Manifest.csv"
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "ACMI conversion did not create a manifest: $manifestPath"
}

$renderArguments = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", $renderScript,
    "-Fps", $Fps,
    "-PlaybackRate", $PlaybackRate,
    "-ReplayLimitSeconds", $ReplayLimitSeconds,
    "-ReplayCsvDirectory", $csvDirectory,
    "-Width", $Width,
    "-Height", $Height,
    "-WarmupSeconds", $WarmupSeconds,
    "-MaxWarmupSeconds", $MaxWarmupSeconds,
    "-EngineRoot", $EngineRoot,
    "-OutputDirectory", $OutputDirectory,
    "-FfmpegPath", $FfmpegPath
)
if ($SkipBuild) {
    $renderArguments += "-SkipBuild"
}
if (-not $ShowWindow) {
    $renderArguments += "-RenderOffscreen"
}
if ($KeepProxy) {
    $renderArguments += "-KeepProxy"
}
if ($PngSequence) {
    $renderArguments += "-PngSequence"
}

Write-Host "Rendering ACMI round to MP4..." -ForegroundColor Cyan
& powershell.exe @renderArguments
if ($LASTEXITCODE -ne 0) {
    throw "Round rendering failed with exit code $LASTEXITCODE. Generated CSV remains at $csvDirectory"
}

$videoPath = Join-Path $OutputDirectory "AirCombatRound.mp4"
if (-not (Test-Path -LiteralPath $videoPath -PathType Leaf)) {
    throw "Rendering finished without an MP4: $videoPath"
}

if (-not $KeepCsv) {
    Remove-Item -LiteralPath $csvDirectory -Recurse -Force
}

Write-Host "ACMI video ready: $videoPath" -ForegroundColor Green
if ($KeepCsv) {
    Write-Host "Converted CSV kept at: $csvDirectory"
}
