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

  # Keep the user-facing Firmware directory synchronized with the actual
  # Keil output. Without this publish step it is easy to repeatedly flash an
  # older HEX even though the IDE reports a successful new build.
  $outputDir = Join-Path $projectDir 'RBLink_DAP_V3.0'
  $firmwareDir = Join-Path (Split-Path -Parent $projectDir) 'Firmware'
  New-Item -ItemType Directory -Path $firmwareDir -Force | Out-Null
  Copy-Item -LiteralPath (Join-Path $outputDir 'RBLink_DAP_V3_0.hex') -Destination $firmwareDir -Force
  Copy-Item -LiteralPath (Join-Path $outputDir 'RBLink_DAP_V3_0.axf') -Destination $firmwareDir -Force
  Write-Host "Published HEX/AXF to $firmwareDir"
} finally {
  Pop-Location
}
