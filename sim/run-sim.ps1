# Build and launch the FlightScnr desktop simulator (Windows).
#
#   .\sim\run-sim.ps1                         # last saved location
#   .\sim\run-sim.ps1 -Lat 27.9755 -Lon -82.5332
#   .\sim\run-sim.ps1 -Scale 0.8 -NoBuild
#   .\sim\run-sim.ps1 -Extra '--tap','8:360:677','--shot','20','--exit-after','25'
#
# Map under the radar (baked by tools\sim_bake_basemap.py before launch):
#   .\sim\run-sim.ps1 -Map vfr                       # FAA VFR chart (US, no key)
#   .\sim\run-sim.ps1 -Map dark -MapMiles 10         # CARTO dark; also light / voyager
#   .\sim\run-sim.ps1 -Map dark -CartoKey <key>      # if CARTO starts requiring a key
#   .\sim\run-sim.ps1 -Map off
# The map covers the current range (or -MapMiles); zooming out past it, or moving
# the center, hides it until you bake again.
#
# Mouse = finger (click = tap, drag = swipe). F12 = screenshot, Esc = quit.
# Settings and caches are kept in .\sim_data\ (delete it to start fresh).
param(
  [double]$Lat = [double]::NaN,
  [double]$Lon = [double]::NaN,
  [double]$Scale = 1.0,
  [switch]$NoBuild,
  [ValidateSet('', 'vfr', 'dark', 'light', 'voyager', 'off')]
  [string]$Map = '',
  [int]$MapMiles = 0,
  [string]$CartoKey = '',
  # Separate settings/map folder, e.g. one per location: -Data sim_data_odessa
  [string]$Data = '',
  [string[]]$Extra = @()
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

# Pick up compilers installed by winget since this shell started.
$env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' +
            [Environment]::GetEnvironmentVariable('Path', 'User')
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue

if (-not $NoBuild) {
  if (-not (Get-Command g++ -ErrorAction SilentlyContinue)) {
    Write-Host 'No g++ found. Install it with:  winget install BrechtSanders.WinLibs.POSIX.UCRT' -ForegroundColor Yellow
    exit 1
  }
  $pio = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\pio.exe'
  if (-not (Test-Path $pio)) { $pio = 'pio' }
  & $pio run -e sim
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if ($Map) {
  # The simulator rewrites sim_data\nvs.txt while running; bake with it closed.
  $running = Get-Process program -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -like '*\.pio\build\sim\program.exe' }
  if ($running) {
    Write-Host 'Closing the running simulator to update its map...'
    $running | Stop-Process -Force
    Start-Sleep -Milliseconds 500
  }
  $bake = @((Join-Path $root 'tools\sim_bake_basemap.py'))
  if ($Map -eq 'off') { $bake += '--off' } else { $bake += @('--style', $Map) }
  if ($MapMiles -gt 0) { $bake += @('--miles', "$MapMiles") }
  if ($CartoKey) { $bake += @('--carto-key', $CartoKey) }
  if ($Data) { $bake += @('--data', (Join-Path $root $Data)) }
  if (-not [double]::IsNaN($Lat) -and -not [double]::IsNaN($Lon)) {
    $bake += @('--lat', "$Lat", '--lon', "$Lon")
  }
  & python @bake
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$argsList = @('--scale', "$Scale")
if ($Data) { $argsList += @('--data', (Join-Path $root $Data)) }
if (-not [double]::IsNaN($Lat) -and -not [double]::IsNaN($Lon)) {
  $argsList += @('--lat', "$Lat", '--lon', "$Lon")
}
$argsList += $Extra
& (Join-Path $root '.pio\build\sim\program.exe') @argsList
