# Makes every library under "<repo>/libraries" visible to Arduino IDE/CLI.
#
# Arduino only searches the sketch folder and the sketchbook "libraries"
# directory. Keeping the libraries in the repository and linking them into the
# sketchbook means the repo stays the single source of truth without copying
# files. Run this after adding or removing a library; the pre-build hook in
# .vscode/arduino.json runs it automatically.

param(
    [string]$SketchbookPath = "$env:USERPROFILE\Documents\Arduino"
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$sourceRoot = Join-Path $root "libraries"
$linkRoot = Join-Path $SketchbookPath "libraries"

if (-not (Test-Path -LiteralPath $sourceRoot)) {
    Write-Error "Library folder not found: $sourceRoot"
    exit 1
}

if (-not (Test-Path -LiteralPath $linkRoot)) {
    New-Item -ItemType Directory -Path $linkRoot -Force | Out-Null
}

$created = 0
$updated = 0
$kept = 0
$removed = 0

$wanted = @{}

foreach ($lib in Get-ChildItem -LiteralPath $sourceRoot -Directory) {
    $wanted[$lib.Name] = $true
    $link = Join-Path $linkRoot $lib.Name

    if (Test-Path -LiteralPath $link) {
        $item = Get-Item -LiteralPath $link -Force
        $isLink = $item.Attributes -band [IO.FileAttributes]::ReparsePoint
        $target = $item.Target | Select-Object -First 1

        if ($isLink -and $target -eq $lib.FullName) {
            $kept++
            continue
        }

        # Stale or unrelated entry: never delete a real directory that may hold
        # libraries installed through the Library Manager.
        if ($isLink) {
            Remove-Item -LiteralPath $link -Force
            $removed++
        }
        else {
            Write-Warning "Kept real directory (not a link), name clash: $link"
            continue
        }
    }

    New-Item -ItemType Junction -Path $link -Target $lib.FullName | Out-Null
    $created++
    Write-Output "linked: $($lib.Name)"
}

# Drop links that pointed into this repo but whose library was deleted.
foreach ($link in Get-ChildItem -LiteralPath $linkRoot -Force) {
    if ($wanted.ContainsKey($link.Name)) { continue }
    if (-not ($link.Attributes -band [IO.FileAttributes]::ReparsePoint)) { continue }

    $target = $link.Target | Select-Object -First 1
    if ($target -and $target.StartsWith($sourceRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $link.FullName -Force
        $removed++
        Write-Output "unlinked (library removed): $($link.Name)"
    }
}

Write-Output "libraries: $created linked, $updated updated, $kept already ok, $removed removed"
