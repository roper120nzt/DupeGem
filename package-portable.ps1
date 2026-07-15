param(
    [string]$InputExe = "release\main.exe",
    [string]$PortableRoot = "portable",
    [string]$Version = "0.4.0"
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$BuiltExe = Join-Path $ProjectRoot $InputExe
$PortableRoot = if([IO.Path]::IsPathRooted($PortableRoot)){
    [IO.Path]::GetFullPath($PortableRoot)
} else {
    [IO.Path]::GetFullPath((Join-Path $ProjectRoot $PortableRoot))
}
$OutDir = Join-Path $PortableRoot 'DupeGem'
if($Version -notmatch '^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?$'){
    throw "Invalid release version: $Version"
}
$ZipPath = Join-Path $PortableRoot "DupeGem-v$Version-portable.zip"

if(-not (Test-Path -LiteralPath $BuiltExe -PathType Leaf)){
    throw "Build $InputExe first with qmake6 main.pro and mingw32-make."
}

$Deploy = Get-Command windeployqt6.exe -ErrorAction SilentlyContinue
if(-not $Deploy){
    $Candidate = 'C:\msys64\mingw64\bin\windeployqt6.exe'
    if(Test-Path -LiteralPath $Candidate){ $Deploy = Get-Item -LiteralPath $Candidate }
}
if(-not $Deploy){ throw 'windeployqt6.exe was not found in PATH or the MSYS2 MinGW64 installation.' }

New-Item -ItemType Directory -Path $PortableRoot -Force | Out-Null
if(Test-Path -LiteralPath $OutDir){
    $ResolvedPortable = (Resolve-Path -LiteralPath $PortableRoot).Path
    $ResolvedOut = (Resolve-Path -LiteralPath $OutDir).Path
    if(-not $ResolvedOut.StartsWith($ResolvedPortable, [StringComparison]::OrdinalIgnoreCase)){
        throw "Refusing to clean unexpected output directory: $ResolvedOut"
    }
    Remove-Item -LiteralPath $OutDir -Recurse -Force
}
New-Item -ItemType Directory -Path $OutDir | Out-Null

$OutExe = Join-Path $OutDir 'DupeGem.exe'
Copy-Item -LiteralPath $BuiltExe -Destination $OutExe
foreach($Asset in @('icon.png','icon.ico')){
    $AssetPath=Join-Path $ProjectRoot "release\$Asset"
    if(Test-Path -LiteralPath $AssetPath){ Copy-Item -LiteralPath $AssetPath -Destination (Join-Path $OutDir $Asset) }
}

Write-Host 'Deploying Qt runtime and plugins...'
& $Deploy.Source --release --no-translations --no-system-d3d-compiler --no-opengl-sw --dir $OutDir $OutExe
if($LASTEXITCODE -ne 0){ throw "windeployqt6 failed with exit code $LASTEXITCODE." }

# DupeGem uses SQLite only. windeployqt deploys every installed SQL backend,
# which pulls in unrelated Firebird, MariaDB, PostgreSQL, and ODBC runtimes.
$SqlDrivers = Join-Path $OutDir 'sqldrivers'
foreach($UnusedDriver in @('qsqlibase.dll','qsqlmysql.dll','qsqlodbc.dll','qsqlpsql.dll')){
    $UnusedPath = Join-Path $SqlDrivers $UnusedDriver
    if(Test-Path -LiteralPath $UnusedPath){ Remove-Item -LiteralPath $UnusedPath -Force }
}

# windeployqt handles Qt. Recursively close the remaining MinGW dependency
# graph (libgcc, libstdc++, image codec libraries, and their dependencies).
$DependencyScript = Join-Path $OutDir 'copy-deps.ps1'
Copy-Item -LiteralPath (Join-Path $ProjectRoot 'copy-deps.ps1') -Destination $DependencyScript
Push-Location $OutDir
try {
    & $DependencyScript -NoGui
    if($LASTEXITCODE -and $LASTEXITCODE -ne 0){ throw "Dependency collection failed with exit code $LASTEXITCODE." }
} finally {
    Pop-Location
}
Remove-Item -LiteralPath $DependencyScript -Force

$Missing = Join-Path $OutDir 'missing-deps.txt'
if(Test-Path -LiteralPath $Missing){
    throw "Some dependencies could not be found. See $Missing"
}

if(Test-Path -LiteralPath $ZipPath){ Remove-Item -LiteralPath $ZipPath -Force }
Compress-Archive -Path (Join-Path $OutDir '*') -DestinationPath $ZipPath -CompressionLevel Optimal

$Files = Get-ChildItem -LiteralPath $OutDir -Recurse -File
$Bytes = ($Files | Measure-Object -Property Length -Sum).Sum
Write-Host ("Portable build ready: {0} files, {1:N1} MB" -f $Files.Count, ($Bytes / 1MB))
Write-Host "Folder: $OutDir"
Write-Host "ZIP:    $ZipPath"
