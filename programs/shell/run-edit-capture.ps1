param(
    [Parameter(Mandatory = $true)][string]$Emu,
    [Parameter(Mandatory = $true)][string]$Boot,
    [Parameter(Mandatory = $true)][string]$Kernel,
    [Parameter(Mandatory = $true)][string]$Gif,
    [string]$Keys = 'editsmk',
    [int]$Seconds = 12,
    # Optional FAT32 image. The editor smoke tests that only draw a screen do
    # not need a card; the ones that open a file do.
    [string]$Sdcard = ''
)

$ErrorActionPreference = 'Stop'

Remove-Item -LiteralPath $Gif -ErrorAction SilentlyContinue

$env:SDL_VIDEODRIVER = 'dummy'
$env:SDL_AUDIODRIVER = 'dummy'

$emuDir = Split-Path -Parent $Emu
$env:PATH = "$emuDir;C:\msys64\mingw64\bin;C:\msys64\usr\bin;$env:PATH"

$keys = "$Keys`n"
$quotedKeys = '"' + ($keys -replace '"', '\"') + '"'

$args = @(
    '-boot', $Boot,
    '-load', "F00000,$Kernel",
    '-autokeys', $quotedKeys,
    '-warp',
    '-gif', $Gif
)
if ($Sdcard -ne '') {
    $args += @('-sdcard', $Sdcard)
}

$p = Start-Process -FilePath $Emu `
    -WorkingDirectory $emuDir `
    -ArgumentList $args `
    -PassThru `
    -WindowStyle Hidden
Start-Sleep -Seconds $Seconds
if (!$p.HasExited) {
    Stop-Process -Id $p.Id -Force
}

if (!(Test-Path -LiteralPath $Gif)) {
    exit 2
}
