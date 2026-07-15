param(
    [string]$Executable = (Join-Path $PSScriptRoot '..\release\main.exe')
)

$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$executablePath = [IO.Path]::GetFullPath($Executable)
$sqlite = 'C:\msys64\mingw64\bin\sqlite3.exe'
if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf)) {
    throw "DupeGem executable not found: $executablePath"
}
if (-not (Test-Path -LiteralPath $sqlite -PathType Leaf)) {
    throw "sqlite3 not found: $sqlite"
}

$runId = [guid]::NewGuid().ToString('N')
$fixture = Join-Path $workspace ".dupegem-integration-$runId"
$results = Join-Path $workspace ".dupegem-integration-results-$runId"
$cache = Join-Path $fixture '.dupegem_cache.sqlite'

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "FAIL: $Message" }
}

function Invoke-Sqlite([string]$Sql) {
    $value = & $sqlite $cache $Sql
    if ($LASTEXITCODE -ne 0) { throw "sqlite3 failed: $Sql" }
    return ($value -join "`n").Trim()
}

function Invoke-Benchmark([string]$Algorithm, [string]$Name, [int]$CancelAfter = 0) {
    $output = Join-Path $results "$Name.json"
    $arguments = @('--benchmark', $fixture, '--algorithm', $Algorithm, '--output', $output)
    if ($CancelAfter -gt 0) { $arguments += @('--cancel-after', $CancelAfter) }
    $process = Start-Process -FilePath $executablePath -ArgumentList $arguments `
        -PassThru -Wait -WindowStyle Hidden
    if ($process.ExitCode -ne 0) {
        throw "DupeGem benchmark '$Name' exited with $($process.ExitCode)"
    }
    return (Get-Content -LiteralPath $output -Raw | ConvertFrom-Json)
}

function Write-Ppm([string]$Path, [int]$Index) {
    $header = [Text.Encoding]::ASCII.GetBytes("P6`n32 24`n255`n")
    $pixels = [byte[]]::new(32 * 24 * 3)
    for ($position = 0; $position -lt $pixels.Length; $position += 3) {
        $pixels[$position] = [byte](($Index * 29 + $position) % 251)
        $pixels[$position + 1] = [byte](($Index * 47 + ($position / 3)) % 253)
        $pixels[$position + 2] = [byte](($Index * 71 + ($position / 7)) % 255)
    }
    $bytes = [byte[]]::new($header.Length + $pixels.Length)
    [Array]::Copy($header, 0, $bytes, 0, $header.Length)
    [Array]::Copy($pixels, 0, $bytes, $header.Length, $pixels.Length)
    [IO.File]::WriteAllBytes($Path, $bytes)
}

try {
    [IO.Directory]::CreateDirectory($fixture) | Out-Null
    [IO.Directory]::CreateDirectory($results) | Out-Null
    for ($index = 0; $index -lt 80; $index++) {
        Write-Ppm (Join-Path $fixture ('image_{0:D4}.ppm' -f $index)) $index
    }

    # Deliberately create the v0.3-era schema. Opening it must migrate in place.
    $oldSchema = @'
CREATE TABLE images(
 path TEXT PRIMARY KEY, mtime INTEGER NOT NULL, size INTEGER NOT NULL,
 width INTEGER, height INTEGER, dhash BLOB, phash BLOB, ahash BLOB,
 whash BLOB, bhash BLOB, md5 TEXT
);
'@
    Invoke-Sqlite $oldSchema | Out-Null
    $exact = Invoke-Benchmark 'md5' 'schema-migration'
    Assert-True ($exact.files -eq 80) 'old cache schema migrated and all files scanned'
    $columns = (Invoke-Sqlite "SELECT group_concat(name, ',') FROM pragma_table_info('images');") -split ','
    foreach ($required in @('quickhash','prehash','prehash_version','blake3','file_identity','file_usn')) {
        Assert-True ($columns -contains $required) "cache migration added $required"
    }

    $cancelled = Invoke-Benchmark 'dhash' 'cancelled-perceptual' 5
    Assert-True ([bool]$cancelled.cancelled) 'benchmark cancellation was reported'
    Assert-True ($cancelled.completed_operations -ge 5) 'cancellation completed the requested checkpoint'
    $partial = [int](Invoke-Sqlite 'SELECT count(*) FROM images WHERE dhash IS NOT NULL;')
    Assert-True ($partial -ge 5 -and $partial -lt 80) 'partial perceptual hashes were persisted'

    $resumed = Invoke-Benchmark 'dhash' 'resumed-perceptual'
    Assert-True (-not [bool]$resumed.cancelled) 'resumed scan completed'
    $complete = [int](Invoke-Sqlite @'
SELECT count(*) FROM images
WHERE dhash IS NOT NULL AND phash IS NOT NULL AND ahash IS NOT NULL
  AND whash IS NOT NULL AND bhash IS NOT NULL;
'@)
    Assert-True ($complete -eq 80) 'resume filled every perceptual hash without losing checkpoints'
    Assert-True ($resumed.completed_operations -eq (80 - $partial)) 'resume skipped cached hashes'

    Rename-Item -LiteralPath (Join-Path $fixture 'image_0001.ppm') -NewName 'renamed.ppm'
    $renamed = Invoke-Benchmark 'whash' 'rename-reuse'
    Assert-True ($renamed.completed_operations -eq 0) 'NTFS file identity reused hashes after rename'
    Assert-True ((Invoke-Sqlite "SELECT count(*) FROM images WHERE path='renamed.ppm';") -eq '1') `
        'cache path updated after rename'
    Assert-True ((Invoke-Sqlite "SELECT count(*) FROM images WHERE path='image_0001.ppm';") -eq '0') `
        'old renamed path was removed'

    Remove-Item -LiteralPath (Join-Path $fixture 'image_0002.ppm')
    $stale = Invoke-Benchmark 'whash' 'stale-cleanup'
    Assert-True ($stale.files -eq 79) 'deleted file omitted from rescan'
    Assert-True ((Invoke-Sqlite 'SELECT count(*) FROM images;') -eq '79') 'stale cache row removed'

    $hardlink = Join-Path $fixture 'hardlink.ppm'
    New-Item -ItemType HardLink -Path $hardlink -Target (Join-Path $fixture 'image_0003.ppm') | Out-Null
    $hardlinks = Invoke-Benchmark 'md5' 'hardlink-filter'
    Assert-True ($hardlinks.hardlinks_skipped -ge 1) 'hardlink aliases skipped before hashing'

    Copy-Item -LiteralPath (Join-Path $fixture 'image_0003.ppm') `
        -Destination (Join-Path $fixture 'exact-copy.ppm')
    $largeBytes = [byte[]]::new(2MB)
    for ($position = 0; $position -lt $largeBytes.Length; $position++) {
        $largeBytes[$position] = [byte](($position * 73 + 19) % 251)
    }
    [IO.File]::WriteAllBytes((Join-Path $fixture 'large-a.bin'), $largeBytes)
    [IO.File]::WriteAllBytes((Join-Path $fixture 'large-b.bin'), $largeBytes)
    $duplicates = Invoke-Benchmark 'md5' 'adaptive-exact-duplicates'
    Assert-True ($duplicates.groups -ge 2) 'small and large exact duplicates formed groups'
    Assert-True ((Invoke-Sqlite @'
SELECT count(*) FROM images
WHERE path IN ('image_0003.ppm','hardlink.ppm','exact-copy.ppm','large-a.bin','large-b.bin')
  AND length(blake3)=32;
'@) -eq '4') 'adaptive exact stages persisted BLAKE3 for true candidates'

    Write-Output ('Integration tests passed: migration, cancellation/resume, rename reuse, ' +
                  'stale cleanup, hardlink filtering, and adaptive exact hashing.')
}
finally {
    foreach ($candidate in @($fixture, $results)) {
        $resolved = [IO.Path]::GetFullPath($candidate)
        $leaf = Split-Path -Leaf $resolved
        Assert-True ($resolved.StartsWith($workspace, [StringComparison]::OrdinalIgnoreCase)) `
            'cleanup remained inside the workspace'
        Assert-True ($leaf.StartsWith('.dupegem-integration', [StringComparison]::Ordinal)) `
            'cleanup target has the expected integration-test prefix'
        if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
    }
}
