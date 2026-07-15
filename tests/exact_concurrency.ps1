param(
    [string]$Executable = (Join-Path $PSScriptRoot '..\release\main.exe'),
    [int]$FileCount = 24,
    [int]$FileSizeMB = 16,
    [string]$FixtureParent = (Join-Path $PSScriptRoot '..')
)

$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$fixtureParentPath = [IO.Path]::GetFullPath($FixtureParent)
if (-not (Test-Path -LiteralPath $fixtureParentPath -PathType Container)) {
    throw "Fixture parent not found: $fixtureParentPath"
}
$executablePath = [IO.Path]::GetFullPath($Executable)
if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf)) {
    throw "DupeGem executable not found: $executablePath"
}
$runId = [guid]::NewGuid().ToString('N')
$fixture = Join-Path $fixtureParentPath ".dupegem-concurrency-$runId"
$reports = [Collections.Generic.List[string]]::new()

try {
    [IO.Directory]::CreateDirectory($fixture) | Out-Null
    $buffer = [byte[]]::new($FileSizeMB * 1MB)
    for ($position = 0; $position -lt $buffer.Length; $position++) {
        $buffer[$position] = [byte](($position * 131 + 17) % 251)
    }
    for ($index = 0; $index -lt $FileCount; $index++) {
        [IO.File]::WriteAllBytes((Join-Path $fixture ('duplicate_{0:D3}.bin' -f $index)), $buffer)
    }

    $measurements = foreach ($threads in @(1, 2, 4, 8)) {
        Get-ChildItem -LiteralPath $fixture -Force -File |
            Where-Object Name -Like '.dupegem_cache.*' | Remove-Item -Force
        $output = Join-Path $workspace ".dupegem-concurrency-$runId-$threads.json"
        $reports.Add($output)
        $env:DUPEGEM_EXACT_THREADS = [string]$threads
        $arguments = @('--benchmark', $fixture, '--algorithm', 'md5', '--output', $output)
        $process = Start-Process -FilePath $executablePath -ArgumentList $arguments `
            -PassThru -Wait -WindowStyle Hidden
        if ($process.ExitCode -ne 0) { throw "Benchmark exited with $($process.ExitCode)" }
        $result = Get-Content -LiteralPath $output -Raw | ConvertFrom-Json
        [pscustomobject]@{
            threads = $threads
            total_ms = $result.total_ms
            hash_ms = $result.decode_hash_ms
            files_per_second = $result.files_per_second
            bytes_read_mb = [Math]::Round($result.bytes_read / 1MB, 1)
        }
    }
    $measurements | Format-Table -AutoSize
}
finally {
    Remove-Item Env:DUPEGEM_EXACT_THREADS -ErrorAction SilentlyContinue
    $resolved = [IO.Path]::GetFullPath($fixture)
    if (-not $resolved.StartsWith($fixtureParentPath, [StringComparison]::OrdinalIgnoreCase) -or
        -not (Split-Path -Leaf $resolved).StartsWith('.dupegem-concurrency-', [StringComparison]::Ordinal)) {
        throw "Refusing to clean unexpected path: $resolved"
    }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
    foreach ($report in $reports) {
        $resolvedReport = [IO.Path]::GetFullPath($report)
        if ($resolvedReport.StartsWith($workspace, [StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path -Leaf $resolvedReport).StartsWith('.dupegem-concurrency-', [StringComparison]::Ordinal) -and
            (Test-Path -LiteralPath $resolvedReport)) {
            Remove-Item -LiteralPath $resolvedReport -Force
        }
    }
}
