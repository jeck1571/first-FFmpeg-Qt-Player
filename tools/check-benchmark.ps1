param(
    [Parameter(Mandatory = $true)]
    [string]$ResultPath,
    [double]$MaxFirstFrameMs = 250,
    [double]$MaxSeekP95Ms = 350,
    [double]$MaxCpuPercent = 85,
    [int]$MaxDroppedFrames = 2
)

$ErrorActionPreference = "Stop"
if (-not (Test-Path -LiteralPath $ResultPath)) {
    throw "Benchmark result does not exist: $ResultPath"
}

$result = Get-Content -LiteralPath $ResultPath -Raw | ConvertFrom-Json
$failures = [System.Collections.Generic.List[string]]::new()

if ($null -eq $result.firstFrameMs -or $result.firstFrameMs -gt $MaxFirstFrameMs) {
    $failures.Add("firstFrameMs=$($result.firstFrameMs) > $MaxFirstFrameMs")
}
if ($null -eq $result.seekP95Ms -or $result.seekP95Ms -gt $MaxSeekP95Ms) {
    $failures.Add("seekP95Ms=$($result.seekP95Ms) > $MaxSeekP95Ms")
}
if ($null -eq $result.avgCpuPercent -or $result.avgCpuPercent -gt $MaxCpuPercent) {
    $failures.Add("avgCpuPercent=$($result.avgCpuPercent) > $MaxCpuPercent")
}
if ($null -eq $result.droppedFrames -or $result.droppedFrames -gt $MaxDroppedFrames) {
    $failures.Add("droppedFrames=$($result.droppedFrames) > $MaxDroppedFrames")
}
if ($result.packetsDropped -ne 0) {
    $failures.Add("packetsDropped=$($result.packetsDropped) != 0")
}

if ($failures.Count -gt 0) {
    throw ("Benchmark thresholds failed:`n" + ($failures -join "`n"))
}

Write-Host ("Benchmark passed: firstFrame={0}ms seekP95={1}ms cpu={2}% dropped={3}" -f `
    $result.firstFrameMs, $result.seekP95Ms, $result.avgCpuPercent, $result.droppedFrames)
