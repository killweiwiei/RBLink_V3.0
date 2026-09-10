param(
  [string]$Keil = 'D:\Keil_v5\UV4\UV4.exe'
)

$ErrorActionPreference = 'Stop'
$projectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectFile = Join-Path $projectDir 'RBLink_DAP_V3.0.uvprojx'
$logFile = Join-Path $projectDir 'build.log'
$outputDir = Join-Path $projectDir 'RBLink_DAP_V3.0'
$outputHex = Join-Path $outputDir 'RBLink_DAP_V3_0.hex'

if (-not (Test-Path -LiteralPath $Keil)) {
  throw "Keil executable not found: $Keil"
}

Push-Location $projectDir
try {
  # UV4 may return before the build worker finishes.  Remove the previous log
  # and generated HEX so neither polling nor publishing can mistake stale
  # output for this build. A rebuild is cheap enough for this firmware and
  # makes the user-facing Firmware directory deterministic.
  Remove-Item -LiteralPath $logFile -Force -ErrorAction SilentlyContinue
  Remove-Item -LiteralPath $outputHex -Force -ErrorAction SilentlyContinue
  & $Keil -r $projectFile -j0 -o $logFile
  $deadline = (Get-Date).AddMinutes(3)
  do {
    Start-Sleep -Milliseconds 500
    $text = if (Test-Path -LiteralPath $logFile) { Get-Content -LiteralPath $logFile -Raw } else { '' }
  } while (($text -notmatch 'Build Time Elapsed:') -and ((Get-Date) -lt $deadline))
  Write-Host $text
  if (($text -match 'Target not created') -or
      ($text -notmatch '"[^"]+"\s+-\s+0 Error\(s\),\s+\d+ Warning\(s\)\.')) { exit 1 }

  # Keep the user-facing Firmware directory synchronized with the actual
  # Keil output. Without this publish step it is easy to repeatedly flash an
  # older HEX even though the IDE reports a successful new build.
  $stable = 0
  $lastSignature = ''
  while (($stable -lt 3) -and ((Get-Date) -lt $deadline)) {
    if (Test-Path -LiteralPath $outputHex) {
      $item = Get-Item -LiteralPath $outputHex
      $signature = "$($item.Length):$($item.LastWriteTimeUtc.Ticks)"
      if ($signature -eq $lastSignature) { $stable++ } else { $stable = 0; $lastSignature = $signature }
    }
    Start-Sleep -Milliseconds 400
  }
  if ($stable -lt 3) { throw 'Keil HEX output did not become stable before the timeout.' }
  $firmwareDir = Join-Path (Split-Path -Parent $projectDir) 'Firmware'
  New-Item -ItemType Directory -Path $firmwareDir -Force | Out-Null
  Copy-Item -LiteralPath $outputHex -Destination $firmwareDir -Force
  Copy-Item -LiteralPath (Join-Path $outputDir 'RBLink_DAP_V3_0.axf') -Destination $firmwareDir -Force
  Write-Host "Published HEX/AXF to $firmwareDir"
} finally {
  Pop-Location
}
