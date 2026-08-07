param(
    [ValidateRange(1, 120)]
    [int]$Fps = 30,

    [ValidateRange(0.25, 4.0)]
    [double]$PlaybackRate = 0.5,

    [ValidateRange(0, 3600)]
    [double]$ReplayLimitSeconds = 0,

    [string]$ReplayCsvDirectory = "",

    [ValidateRange(320, 7680)]
    [int]$Width = 1920,

    [ValidateRange(240, 4320)]
    [int]$Height = 1080,

    [ValidateRange(0, 120)]
    [double]$WarmupSeconds = 8,

    [ValidateRange(10, 600)]
    [double]$MaxWarmupSeconds = 90,

    [string]$EngineRoot = "F:\epic\games\UE_5.6",

    [string]$OutputDirectory = "",

    [string]$FfmpegPath = "ffmpeg",

    [switch]$SkipBuild,

    [switch]$KeepProxy,

    [switch]$RenderOffscreen,

    [switch]$PngSequence
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$projectFile = Join-Path $projectRoot "AirCombatSim.uproject"
$buildScript = Join-Path $EngineRoot "Engine\Build\BatchFiles\Build.bat"
$editorExecutable = Join-Path $EngineRoot "Engine\Binaries\Win64\UnrealEditor.exe"

if (-not (Test-Path -LiteralPath $projectFile)) {
    throw "Project not found: $projectFile"
}
if (-not (Test-Path -LiteralPath $editorExecutable)) {
    throw "Unreal Editor not found. Pass the UE 5.6 directory with -EngineRoot."
}
if (-not [string]::IsNullOrWhiteSpace($ReplayCsvDirectory)) {
    $ReplayCsvDirectory = [System.IO.Path]::GetFullPath($ReplayCsvDirectory)
    $runtimeManifest = Join-Path $ReplayCsvDirectory "Match_Manifest.csv"
    if (-not (Test-Path -LiteralPath $runtimeManifest)) {
        throw "Replay CSV manifest not found: $runtimeManifest"
    }
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $OutputDirectory = Join-Path $projectRoot "Saved\RoundRenders\Round_$stamp"
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
$framesDirectory = Join-Path $OutputDirectory "Frames"
$videoFile = Join-Path $OutputDirectory "AirCombatRound.mp4"
$completionFile = Join-Path $framesDirectory "CaptureComplete.txt"
$failureFile = Join-Path $framesDirectory "CaptureFailed.txt"
New-Item -ItemType Directory -Path $framesDirectory -Force | Out-Null
Remove-Item -LiteralPath $completionFile, $failureFile -Force -ErrorAction SilentlyContinue

$ffmpegCommand = Get-Command $FfmpegPath -ErrorAction SilentlyContinue
$directEncode = -not $PngSequence -and $null -ne $ffmpegCommand
if (-not $directEncode -and -not $PngSequence) {
    Write-Warning "ffmpeg was not found before launch; falling back to the PNG sequence workflow."
}

if (-not $SkipBuild) {
    if (-not (Test-Path -LiteralPath $buildScript)) {
        throw "Unreal build script not found: $buildScript"
    }

    Write-Host "Building AirCombatSimEditor..." -ForegroundColor Cyan
    & $buildScript AirCombatSimEditor Win64 Development $projectFile -WaitMutex -NoHotReloadFromIDE
    if ($LASTEXITCODE -ne 0) {
        throw "Unreal build failed with exit code $LASTEXITCODE."
    }
}

Write-Host "Rendering the complete CSV round at ${Width}x${Height}, $Fps FPS..." -ForegroundColor Cyan
$launchArguments = @(
    $projectFile,
    "/Game/Untitled",
    "-game",
    "-windowed",
    "-ForceRes",
    "-ResX=$Width",
    "-ResY=$Height",
    "-NoVSync",
    "-NoSplash",
    "-NoSound",
    "-unattended",
    "-AutoRecordRound",
    "-RecordFPS=$Fps",
    "-ReplayRate=$PlaybackRate",
    "-RecordReplayLimit=$ReplayLimitSeconds",
    "-RecordWarmup=$WarmupSeconds",
    "-RecordMaxWarmup=$MaxWarmupSeconds",
    "-RecordOutputDir=`"$framesDirectory`""
)
if (-not [string]::IsNullOrWhiteSpace($ReplayCsvDirectory)) {
    $launchArguments += "-ReplayCsvDir=`"$ReplayCsvDirectory`""
    Write-Host "Using generated replay CSV: $ReplayCsvDirectory" -ForegroundColor Cyan
}
if ($RenderOffscreen) {
    $launchArguments += "-RenderOffscreen"
}
if ($directEncode) {
    $launchArguments += "-RecordFfmpeg=`"$($ffmpegCommand.Source)`""
    $launchArguments += "-RecordVideoFile=`"$videoFile`""
    Write-Host "Frames will be streamed directly to ffmpeg; no PNG sequence will be stored." -ForegroundColor Cyan
}

$proxyVariableNames = @("http_proxy", "https_proxy", "all_proxy")
$savedProxyValues = @{}
if (-not $KeepProxy) {
    foreach ($proxyVariableName in $proxyVariableNames) {
        $savedProxyValues[$proxyVariableName] = [Environment]::GetEnvironmentVariable($proxyVariableName, "Process")
        [Environment]::SetEnvironmentVariable($proxyVariableName, $null, "Process")
    }
    Write-Host "Launching Unreal without inherited HTTP proxy variables for Cesium connectivity." -ForegroundColor Cyan
}

try {
    if ($RenderOffscreen) {
        $editorProcess = Start-Process -FilePath $editorExecutable -ArgumentList $launchArguments -PassThru -WindowStyle Hidden
    }
    else {
        $editorProcess = Start-Process -FilePath $editorExecutable -ArgumentList $launchArguments -PassThru
    }
}
finally {
    if (-not $KeepProxy) {
        foreach ($proxyVariableName in $proxyVariableNames) {
            [Environment]::SetEnvironmentVariable(
                $proxyVariableName,
                $savedProxyValues[$proxyVariableName],
                "Process"
            )
        }
    }
}
$editorProcess.WaitForExit()
if ($editorProcess.ExitCode -ne 0) {
    throw "Unreal render process failed with exit code $($editorProcess.ExitCode). Frames, if any, remain in $framesDirectory"
}

# UnrealEditor.exe may hand the game window to a child process and return early.
# The replay manager writes this sentinel only after its final black frame is processed.
$captureDeadline = (Get-Date).AddHours(3)
while (-not (Test-Path -LiteralPath $completionFile)) {
    if (Test-Path -LiteralPath $failureFile) {
        $failureReason = Get-Content -LiteralPath $failureFile -Raw
        throw "Round capture aborted before frame zero: $failureReason"
    }
    if ((Get-Date) -ge $captureDeadline) {
        throw "Timed out waiting for round capture. Check Saved\Logs\AirCombatSim.log."
    }
    Start-Sleep -Milliseconds 500
}

if ($directEncode) {
    if (-not (Test-Path -LiteralPath $videoFile) -or (Get-Item -LiteralPath $videoFile).Length -le 0) {
        throw "Direct encoding completed without a valid MP4. Check Saved\Logs\AirCombatSim.log."
    }
    Write-Host "Video ready: $videoFile" -ForegroundColor Green
    Write-Host "No intermediate PNG sequence was created." -ForegroundColor Green
    exit 0
}

$firstFrame = Join-Path $framesDirectory "Frame_000000.png"
if (-not (Test-Path -LiteralPath $firstFrame)) {
    throw "No frames were produced. Check Saved\Logs\AirCombatSim.log."
}

# A Slate window resize can very occasionally consume one screenshot request.
# Fill any numeric gap with the previous frame so image-sequence encoding remains contiguous.
$capturedFrames = Get-ChildItem -LiteralPath $framesDirectory -Filter "Frame_*.png" |
    Sort-Object Name
$lastFrameNumber = [int]($capturedFrames[-1].BaseName -replace '^Frame_', '')
for ($frameNumber = 0; $frameNumber -le $lastFrameNumber; $frameNumber++) {
    $expectedFrame = Join-Path $framesDirectory ("Frame_{0:D6}.png" -f $frameNumber)
    if (-not (Test-Path -LiteralPath $expectedFrame)) {
        if ($frameNumber -eq 0) {
            throw "The first captured frame is missing."
        }
        $previousFrame = Join-Path $framesDirectory ("Frame_{0:D6}.png" -f ($frameNumber - 1))
        Copy-Item -LiteralPath $previousFrame -Destination $expectedFrame
        Write-Warning "Filled missing frame $frameNumber with the preceding frame."
    }
}

if (-not $ffmpegCommand) {
    Write-Warning "Rendering succeeded, but ffmpeg was not found. PNG frames remain at: $framesDirectory"
    Write-Host "Install ffmpeg, then encode with:" -ForegroundColor Yellow
    Write-Host "ffmpeg -framerate $Fps -i `"$framesDirectory\Frame_%06d.png`" -c:v libx264 -crf 18 -pix_fmt yuv420p `"$videoFile`""
    exit 2
}

Write-Host "Encoding H.264 MP4..." -ForegroundColor Cyan
$framePattern = Join-Path $framesDirectory "Frame_%06d.png"
& $ffmpegCommand.Source -y -framerate $Fps -start_number 0 -i $framePattern `
    -c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p -movflags +faststart $videoFile
if ($LASTEXITCODE -ne 0) {
    throw "ffmpeg failed with exit code $LASTEXITCODE. PNG frames remain in $framesDirectory"
}

Write-Host "Video ready: $videoFile" -ForegroundColor Green
Write-Host "Source frames kept at: $framesDirectory"
