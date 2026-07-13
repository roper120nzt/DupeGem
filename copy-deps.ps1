# copy-deps.ps1 — Collect all non-system DLL dependencies for a Qt/MinGW app
param([switch]$NoGui)

if(-not $NoGui){ Add-Type -AssemblyName System.Windows.Forms | Out-Null }
$ErrorActionPreference = 'Stop'

function Msg($text,$title='Info'){
    if($NoGui){ Write-Host "${title}: $text" }
    else { [System.Windows.Forms.MessageBox]::Show($text,$title,'OK','Information') | Out-Null }
}
function Err($text){
    if($NoGui){ Write-Error $text }
    else { [System.Windows.Forms.MessageBox]::Show($text,'Error','OK','Error') | Out-Null }
    exit 1
}
function FirstExisting([string[]]$paths){ foreach($p in $paths){ if($p -and (Test-Path $p)){ return $p } } return $null }

# Script folder
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
Set-Location $ScriptDir

# Find EXE
$Exe = Get-ChildItem -Filter *.exe | Select-Object -First 1
if(-not $Exe){ Err "No .exe found in:`n$ScriptDir" }

$OutDir = $ScriptDir

# Find tools
$MSYSBin = FirstExisting @("C:\msys64\mingw64\bin","C:\msys64\ucrt64\bin")
$ntldd   = $null; if($MSYSBin){ $p=Join-Path $MSYSBin 'ntldd.exe'; if(Test-Path $p){ $ntldd=$p } }
$objdump = $null; if($MSYSBin){ $p=Join-Path $MSYSBin 'objdump.exe'; if(Test-Path $p){ $objdump=$p } }
if(-not $ntldd){ $c=Get-Command ntldd -ErrorAction SilentlyContinue; if($c){ $ntldd=$c.Source } }
if(-not $objdump){ $c=Get-Command objdump -ErrorAction SilentlyContinue; if($c){ $objdump=$c.Source } }
if(-not $ntldd -and -not $objdump){ Err "Need ntldd.exe or objdump.exe in PATH or in MSYS2 (mingw64/ucrt64\bin)." }

# Search paths for DLLs
$SearchRoots = @()
if($MSYSBin){ $SearchRoots += $MSYSBin }
$SearchRoots += ($env:PATH -split ';' | Where-Object { $_ -and (Test-Path $_) })

# Skip list (system DLLs)
$SysDLL = '(?i)^(api-ms-win-.*|ext-ms-win-.*|KERNEL32|KERNELBASE|ntdll|USER32|GDI32|ADVAPI32|SHELL32|OLE32|OLEAUT32|combase|RPCRT4|SHCORE|SHLWAPI|SETUPAPI|IMM32|USP10|win32u|WTSAPI32|CRYPT32|CRYPTSP|bcrypt|ncrypt|msvcp_win|WS2_32|COMDLG32|COMCTL32|MSVCRT|AUTHZ|MPR|NETAPI32|USERENV|VERSION|WINMM|DWRITE|D3D9|D3D11|D3D12|D3Dcompiler_47|DXGI|UXTHEME|DNSAPI|IPHLPAPI|SECUR32|WINHTTP|DWMAPI)\.dll$'

# Set of all shipped binaries
$Targets = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
Get-ChildItem -Recurse -Include *.exe,*.dll | ForEach-Object { [void]$Targets.Add($_.FullName) }

function Get-ImportedDLLs([string]$file){
    $names=@()
    if($ntldd){
        $lines = & $ntldd -R "$file" 2>$null
        foreach($line in $lines){
            if($line -match '([A-Za-z0-9_\-\.]+\.dll)'){ $names += $Matches[1] }
        }
    } else {
        $lines = & $objdump -p "$file" 2>$null | Where-Object { $_ -match 'DLL Name:' }
        foreach($l in $lines){
            $n = ($l -replace '^\s*DLL Name:\s*','').Trim()
            if($n){ $names += $n }
        }
    }
    return $names
}

function Find-FirstOnPaths([string]$name,[string[]]$roots){
    foreach($r in $roots){
        $cand = Join-Path $r $name
        if(Test-Path $cand){ return $cand }
    }
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if($cmd){ return $cmd.Source }
    return $null
}

# Walk each binary once and enqueue newly copied libraries. The previous
# fixed-pass implementation re-ran objdump over every large Qt DLL up to six
# times, which made packaging take several minutes.
$CopiedTotal=0
$MissingList = @()
$Processed = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
$Queue = New-Object 'System.Collections.Generic.Queue[string]'
foreach($target in @($Targets)){ $Queue.Enqueue($target) }

while($Queue.Count -gt 0){
    $fromBinary = $Queue.Dequeue()
    if(-not $Processed.Add($fromBinary)){ continue }
    foreach($dll in (Get-ImportedDLLs $fromBinary)){
        if(-not $dll -or $dll -match $SysDLL){ continue }
        $dest = Join-Path $OutDir $dll
        if(Test-Path -LiteralPath $dest){ continue }
        $src = Find-FirstOnPaths $dll $SearchRoots
        if($src){
            Copy-Item -LiteralPath $src -Destination $dest
            $CopiedTotal++
            [void]$Targets.Add($dest)
            $Queue.Enqueue($dest)
        } else {
            $MissingList += "$dll (needed by $([IO.Path]::GetFileName($fromBinary)))"
        }
    }
}

if($MissingList.Count -gt 0){
    $MissingList | Sort-Object -Unique | Out-File -Encoding ascii (Join-Path $OutDir 'missing-deps.txt')
    Msg "Copied $CopiedTotal file(s). Some DLLs not found. See missing-deps.txt." 'Done (with warnings)'
} else {
    Msg "Dependency copy complete. $CopiedTotal file(s) copied." 'Done'
}
