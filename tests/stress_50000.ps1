param(
    [string]$Executable = (Join-Path $PSScriptRoot '..\release\main.exe'),
    [int]$FileCount = 50000
)

$ErrorActionPreference = 'Stop'
if ($FileCount -lt 1000) { throw 'FileCount must be at least 1,000.' }
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$executablePath = [IO.Path]::GetFullPath($Executable)
if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf)) {
    throw "DupeGem executable not found: $executablePath"
}
$runId = [guid]::NewGuid().ToString('N')
$fixture = Join-Path $workspace ".dupegem-stress-$runId"
$coldReport = Join-Path $workspace ".dupegem-stress-cold-$runId.json"
$warmReport = Join-Path $workspace ".dupegem-stress-warm-$runId.json"

function Invoke-Benchmark([string]$Output) {
    $arguments = @('--benchmark', $fixture, '--algorithm', 'md5', '--output', $Output)
    $process = Start-Process -FilePath $executablePath -ArgumentList $arguments `
        -PassThru -Wait -WindowStyle Hidden
    if ($process.ExitCode -ne 0) { throw "Benchmark exited with $($process.ExitCode)" }
    return (Get-Content -LiteralPath $Output -Raw | ConvertFrom-Json)
}

try {
    [IO.Directory]::CreateDirectory($fixture) | Out-Null
    $buffer = [byte[]]::new(64)
    for ($index = 0; $index -lt $FileCount; $index++) {
        [Array]::Clear($buffer, 0, $buffer.Length)
        [BitConverter]::GetBytes([long]$index).CopyTo($buffer, 0)
        for ($position = 8; $position -lt $buffer.Length; $position++) {
            $buffer[$position] = [byte](($index * 31 + $position * 17) % 251)
        }
        [IO.File]::WriteAllBytes((Join-Path $fixture ('file_{0:D6}.bin' -f $index)), $buffer)
    }

    $cold = Invoke-Benchmark $coldReport
    $warm = Invoke-Benchmark $warmReport
    if ($cold.files -ne $FileCount -or $warm.files -ne $FileCount) {
        throw "Expected $FileCount files; cold=$($cold.files), warm=$($warm.files)"
    }
    if ($warm.completed_operations -ne 0 -or $warm.cache_hits -ne $FileCount) {
        throw 'Warm rescan recalculated work that should have come from cache.'
    }
    [pscustomobject]@{
        files = $FileCount
        cold_total_ms = $cold.total_ms
        cold_files_per_second = $cold.files_per_second
        cold_bytes_read = $cold.bytes_read
        warm_total_ms = $warm.total_ms
        warm_files_per_second = $warm.files_per_second
        warm_cache_hit_rate = $warm.cache_hit_rate
        warm_peak_memory_mb = [Math]::Round($warm.peak_memory_bytes / 1MB, 1)
    } | Format-List
}
finally {
    $resolved = [IO.Path]::GetFullPath($fixture)
    $leaf = Split-Path -Leaf $resolved
    if (-not $resolved.StartsWith($workspace, [StringComparison]::OrdinalIgnoreCase) -or
        -not $leaf.StartsWith('.dupegem-stress-', [StringComparison]::Ordinal)) {
        throw "Refusing to clean unexpected path: $resolved"
    }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
    foreach ($report in @($coldReport, $warmReport)) {
        $resolvedReport = [IO.Path]::GetFullPath($report)
        if ($resolvedReport.StartsWith($workspace, [StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path -Leaf $resolvedReport).StartsWith('.dupegem-stress-', [StringComparison]::Ordinal) -and
            (Test-Path -LiteralPath $resolvedReport)) {
            Remove-Item -LiteralPath $resolvedReport -Force
        }
    }
}
