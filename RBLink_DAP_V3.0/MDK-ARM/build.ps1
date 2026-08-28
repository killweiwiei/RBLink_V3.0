param(
  [string]$Keil = 'D:\Keil_v5\UV4\UV4.exe'
)

$ErrorActionPreference = 'Stop'
$projectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectFile = Join-Path $projectDir 'RBLink_DAP_V3.0.uvprojx'
$logFile = Join-Path $projectDir 'build.log'

if (-not (Test-Path -LiteralPath $Keil)) {
  throw "Keil executable not found: $Keil"
}

Push-Location $projectDir
try {
  # UV4 may return before the build worker finishes.  Remove the previous log
  # so the polling loop cannot mistake a stale successful build for this one.
  Remove-Item -LiteralPath $logFile -Force -ErrorAction SilentlyContinue
  & $Keil -b $projectFile -j0 -o $logFile
  $deadline = (Get-Date).AddMinutes(3)
  do {
    Start-Sleep -Milliseconds 500
    $text = if (Test-Path -LiteralPath $logFile) { Get-Content -LiteralPath $logFile -Raw } else { '' }
  } while (($text -notmatch '\d+ Error\(s\)') -and ((Get-Date) -lt $deadline))
  Write-Host $text
  if ($text -notmatch '0 Error\(s\)') { exit 1 }
} finally {
  Pop-Location
}
