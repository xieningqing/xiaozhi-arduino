# Generates the IntelliSense compilation database for this Arduino sketch.
#
# Why a compilation database at all:
#   ESP-IDF spreads its headers over directories nested 2..6 levels deep, and the
#   Arduino core only builds because the toolchain passes ~400 -I flags in a
#   specific direction. No wildcard reproduces that: a recursive "include/**"
#   lets IDF's own string.h/stdio.h shadow the real ones, and pruning the list
#   changes header precedence. The list has to be materialised as the compiler
#   receives it, and compile_commands.json is the standard place for it.
#
# Where it goes:
#   .vscode/intellisense_compile_commands.json -- NOT inside the build directory.
#   Earlier versions wrote it under .arduino-build/, which the build tooling
#   wipes; when the file vanished, cpptools silently fell back to the short
#   includePath and every header reported "#include error". Keeping it outside
#   the build tree makes it stable across clean builds.
#
# It is a generated artifact: never edit by hand, and it is git-ignored.
# Refreshed by the postbuild hook in .vscode/arduino.json.

param(
    [string]$BuildDir = ".arduino-build"
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$sketch = Join-Path $root "firmware\xiaozhi"
$rawDb = Join-Path $root (Join-Path $BuildDir "compile_commands.json")
$outDb = Join-Path $root ".vscode\intellisense_compile_commands.json"

if (-not (Test-Path -LiteralPath $rawDb)) {
    # Do not delete a previously generated file here: the build directory may
    # have just been cleaned, and removing the database would break IntelliSense
    # until the next successful build.
    Write-Output "No build database at $rawDb; keeping existing generated file."
    exit 0
}

$utf8 = New-Object System.Text.UTF8Encoding($false)

function Expand-Flags {
    param([string[]]$ArgList, [ref]$CompilerBox)

    $flat = New-Object System.Collections.Generic.List[string]
    foreach ($a in $ArgList) {
        if ($a -like '@*') {
            $f = $a.Substring(1)
            if (Test-Path -LiteralPath $f) {
                foreach ($t in ((Get-Content -Raw -LiteralPath $f) -split '\s+' | Where-Object { $_ })) {
                    $flat.Add($t)
                }
            }
        }
        else { $flat.Add($a) }
    }

    if ($flat.Count -gt 0) { $CompilerBox.Value = $flat[0] }

    $keep = New-Object System.Collections.Generic.List[string]
    $prefix = ""
    for ($i = 0; $i -lt $flat.Count; $i++) {
        $a = $flat[$i]
        if ($a -eq '-iprefix') { $prefix = $flat[$i + 1]; continue }
        if ($a -eq '-iwithprefixbefore') {
            $keep.Add("-I" + ($prefix.TrimEnd('/') + '/' + $flat[$i + 1]))
            continue
        }
        if ($a -like '-I*' -or $a -like '-D*' -or $a -like '-U*' -or
            $a -like '-std=*' -or $a -in '-mlongcalls', '-mdisable-hardware-atomics') {
            $keep.Add($a)
        }
    }
    return $keep
}

$buildSketch = Join-Path $root (Join-Path $BuildDir "sketch")
$db = Get-Content -Raw -LiteralPath $rawDb | ConvertFrom-Json

$compilerBox = [ref]$null
$commonArgs = $null
foreach ($e in $db) {
    if (-not $e.file -or $e.file -notlike "$buildSketch*") { continue }
    $kept = Expand-Flags -ArgList $e.arguments -CompilerBox $compilerBox
    if ($kept.Count -gt 0 -and (-not $commonArgs -or $kept.Count -gt $commonArgs.Count)) {
        $commonArgs = $kept
    }
}

$compiler = $compilerBox.Value
if (-not $compiler -or -not $commonArgs) {
    Write-Error "Could not derive compiler or flags from $rawDb"
    exit 1
}

# Every file the editor can open needs an entry: cpptools applies database flags
# only to the files named in the database. Headers therefore must be listed too,
# or they fall back to the short includePath and report missing includes.
$entries = New-Object System.Collections.Generic.List[object]
$files = Get-ChildItem -LiteralPath $sketch -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Extension -in '.cpp', '.ino', '.h', '.hpp' } |
    Sort-Object FullName
foreach ($f in $files) {
    $entries.Add([ordered]@{
        directory = $root
        file      = $f.FullName
        arguments = @($compiler) + @($commonArgs)
    })
}

[System.IO.File]::WriteAllText($outDb, ($entries | ConvertTo-Json -Depth 6), $utf8)
$kb = [math]::Round((Get-Item -LiteralPath $outDb).Length / 1KB, 0)
Write-Output "generated: $outDb ($($entries.Count) entries, $kb KB)"