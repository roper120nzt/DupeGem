param(
    [string]$Executable = (Join-Path $PSScriptRoot 'release\main.exe'),
    [string]$Dataset = 'L:\FILL',
    [ValidateRange(1, 20)][int]$Rounds = 1,
    [ValidateRange(60, 86400)][int]$TimeoutSeconds = 10800
)

# Compares the normal DupeGem build at 8, 10, and 12 hash/decode workers.
# Pass -Rounds 3 to rotate each worker count through every run position and
# reduce ordinary Windows filesystem-cache ordering bias.
$ErrorActionPreference = 'Stop'
$threadCounts = @(8, 10, 12)
$runner = Join-Path $PSScriptRoot 'benchmark-tools\benchmark_dupegem.ps1'

if (-not (Test-Path -LiteralPath $Executable)) { throw "Executable not found: $Executable" }
if (-not (Test-Path -LiteralPath $Dataset -PathType Container)) { throw "Dataset folder not found: $Dataset" }
if (-not (Test-Path -LiteralPath $runner -PathType Leaf)) { throw "Benchmark harness not found: $runner" }

$timestamp = Get-Date -Format 'yyyy-MM-dd-HHmmss'
$resultDirectory = Join-Path $PSScriptRoot "benchmark-results\thread-counts-$timestamp"
New-Item -ItemType Directory -Path $resultDirectory -Force | Out-Null

$previousThreadOverride = [Environment]::GetEnvironmentVariable('DUPEGEM_THREADS', 'Process')
$results = [System.Collections.Generic.List[object]]::new()

try {
    for ($round = 0; $round -lt $Rounds; ++$round) {
        # Rotate 8 → 10 → 12 across rounds so a worker count is not always
        # first (coldest) or last (warmest) in the sequence.
        for ($position = 0; $position -lt $threadCounts.Count; ++$position) {
            $threads = $threadCounts[($round + $position) % $threadCounts.Count]
            $env:DUPEGEM_THREADS = [string]$threads
            $metricsPath = Join-Path $resultDirectory ("dhash-threads-{0}-round-{1}.json" -f $threads, ($round + 1))

            Write-Host "Running dHash (256-bit, distance 4): $threads workers, round $($round + 1)/$Rounds"
            & $runner -Executable $Executable -Dataset $Dataset -Algorithm 0 -MetricsFile $metricsPath -TimeoutSeconds $TimeoutSeconds
            if (-not (Test-Path -LiteralPath $metricsPath -PathType Leaf)) {
                throw "Benchmark harness did not create metrics for $threads workers in round $($round + 1)."
            }

            $metrics = Get-Content -LiteralPath $metricsPath -Raw | ConvertFrom-Json
            $results.Add([pscustomobject]@{
                Threads = $threads
                Round = $round + 1
                WallSeconds = [double]$metrics.WallSeconds
                CpuSeconds = [double]$metrics.CpuSeconds
                ReadBytes = $metrics.ReadBytes
                PeakWorkingSetBytes = $metrics.PeakWorkingSetBytes
                FilesProcessed = $metrics.FilesProcessed
                Groups = $metrics.Groups
                MetricsFile = $metricsPath
            })
        }
    }
}
finally {
    [Environment]::SetEnvironmentVariable('DUPEGEM_THREADS', $previousThreadOverride, 'Process')
}

$summary = foreach ($threads in $threadCounts) {
    $runs = @($results | Where-Object Threads -eq $threads)
    if ($runs.Count -eq 0) { continue }
    $meanWall = ($runs | Measure-Object WallSeconds -Average).Average
    $meanCpu = ($runs | Measure-Object CpuSeconds -Average).Average
    $meanRate = if ($meanWall -gt 0) { ($runs | Measure-Object FilesProcessed -Sum).Sum / ($meanWall * $runs.Count) } else { 0 }
    [pscustomobject]@{
        Threads = $threads
        Runs = $runs.Count
        MeanWallSeconds = [Math]::Round($meanWall, 3)
        MeanCpuSeconds = [Math]::Round($meanCpu, 3)
        MeanImagesPerSecond = [Math]::Round($meanRate, 2)
        Groups = ($runs | Select-Object -First 1).Groups
        FilesProcessed = ($runs | Select-Object -First 1).FilesProcessed
    }
}

$summary | Format-Table -AutoSize
$summaryPath = Join-Path $resultDirectory 'summary.json'
$summary | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $summaryPath -Encoding utf8
Write-Host "Saved detailed results to: $resultDirectory"
