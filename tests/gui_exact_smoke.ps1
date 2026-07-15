param(
    [string]$Executable = (Join-Path $PSScriptRoot '..\release\main.exe')
)

$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$executablePath = [IO.Path]::GetFullPath($Executable)
$sqlite = 'C:\msys64\mingw64\bin\sqlite3.exe'
$fixture = Join-Path $workspace ('.dupegem-gui-exact-' + [guid]::NewGuid().ToString('N'))
$process = $null
try {
    [IO.Directory]::CreateDirectory($fixture) | Out-Null
    foreach ($size in @(100KB, 2MB)) {
        $bytes = [byte[]]::new($size)
        for ($position = 0; $position -lt $bytes.Length; $position++) {
            $bytes[$position] = [byte](($position * 97 + $size) % 251)
        }
        $label = if ($size -lt 1MB) { 'small' } else { 'large' }
        [IO.File]::WriteAllBytes((Join-Path $fixture "$label-a.jpg"), $bytes)
        [IO.File]::WriteAllBytes((Join-Path $fixture "$label-b.jpg"), $bytes)
    }
    # HashAlgo::MD5 retains its legacy numeric ID (5) for command-line compatibility.
    $process = Start-Process -FilePath $executablePath -ArgumentList @($fixture, '5') `
        -PassThru -WindowStyle Hidden
    $cache = Join-Path $fixture '.dupegem_cache.sqlite'
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    $hashed = 0
    while ([DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 250
        $process.Refresh()
        if ($process.HasExited) { throw "GUI exited early with code $($process.ExitCode)" }
        if (-not (Test-Path -LiteralPath $cache -PathType Leaf)) { continue }
        $query = & $sqlite $cache 'SELECT count(*) FROM images WHERE length(blake3)=32;' 2>$null
        if ($LASTEXITCODE -eq 0 -and $query) { $hashed = [int]$query }
        if ($hashed -eq 4) { break }
    }
    if ($hashed -ne 4) { throw "GUI exact pipeline persisted $hashed of 4 BLAKE3 candidates." }
    Write-Output 'GUI exact pipeline smoke test passed for small and large duplicate groups.'
}
finally {
    if ($process) {
        $process.Refresh()
        if (-not $process.HasExited) { Stop-Process -Id $process.Id }
    }
    $resolved = [IO.Path]::GetFullPath($fixture)
    if (-not $resolved.StartsWith($workspace, [StringComparison]::OrdinalIgnoreCase) -or
        -not (Split-Path -Leaf $resolved).StartsWith('.dupegem-gui-exact-', [StringComparison]::Ordinal)) {
        throw "Refusing to clean unexpected path: $resolved"
    }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
