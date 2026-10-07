# SPDX-License-Identifier: GPL-3.0-or-later
#
# Silent Hill save converter: PSX / DuckStation memory cards <-> PC port saves.
# Normally started by dragging files onto "SH Save Converter.bat".
#
#   PSX card(s) only          -> PC cards (0.MCD, 8.MCD, ...) in "SH saves for PC"
#   PC N.MCD files / save dir -> PSX cards ("Silent Hill (USA)_1.mcd", ...) in "SH saves for PSX"
#   PC + one PSX card         -> asks which way to merge
#
# Inputs are never modified; results always go to a new folder.

[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Paths,
    [ValidateSet('auto', 'topc', 'topsx', 'pc-into-psx', 'psx-into-pc')][string]$Mode = 'auto',
    [ValidateSet('usa', 'eur', 'jpn')][string]$Region = '',
    [string]$OutDir = '',
    [switch]$NoPause,
    [switch]$Info
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Add-Type -Path (Join-Path $here 'ShSaveCard.cs')

$interactive = -not $NoPause
function Finish([int]$code) {
    if ($interactive) { Write-Host ''; Read-Host 'Press Enter to close' | Out-Null }
    exit $code
}

function Load-Card([string]$path) {
    [ShSaveCard.PsxCard]::Load([IO.File]::ReadAllBytes($path))
}

function Region-Of([string]$r) {
    switch ($r) { 'eur' { [ShSaveCard.Region]::Eur } 'jpn' { [ShSaveCard.Region]::Jpn } default { [ShSaveCard.Region]::Usa } }
}

function New-OutDir([string]$near, [string]$name) {
    if ($OutDir) { $d = $OutDir } else {
        $d = Join-Path $near $name
        $n = 2
        while (Test-Path -LiteralPath $d) { $d = Join-Path $near ("$name ($n)"); $n++ }
    }
    New-Item -ItemType Directory -Force -Path $d | Out-Null
    $d
}

function Show-Log($res) { foreach ($l in $res.Log) { Write-Host $l } }

function Card-Region($card) {
    foreach ($f in $card.Files) {
        $r = [ShSaveCard.Region]::Usa; $i = 0
        if ([ShSaveCard.ShFile]::Parse($f.Name, [ref]$r, [ref]$i)) { return $r }
    }
    $null
}

# Each region's game only sees saves named for it (BASLUS / BESLES / BISLPM), so ask
# rather than silently defaulting a Japanese or European player to USA names.
function Ask-Region {
    if ($Region) { return Region-Of $Region }
    if (-not $interactive) { return [ShSaveCard.Region]::Usa }
    Write-Host 'Which Silent Hill disc will play these saves?'
    Write-Host '  1) USA (SLUS-00707)   [Enter]'
    Write-Host '  2) Europe (SLES-01514)'
    Write-Host '  3) Japan (SLPM-86192 and its reissues)'
    switch (Read-Host 'Choose 1, 2 or 3') {
        '2' { return [ShSaveCard.Region]::Eur }
        '3' { return [ShSaveCard.Region]::Jpn }
        default { return [ShSaveCard.Region]::Usa }
    }
}

Write-Host 'Silent Hill save converter (PSX / DuckStation <-> PC port)'
Write-Host ''

if (-not $Paths -or $Paths.Count -eq 0) {
    Write-Host 'Drag one or more files onto "SH Save Converter.bat":'
    Write-Host '  - a DuckStation / PSX memory card (.mcd, .mcr, .mc, .gme, .vmp)  -> PC saves'
    Write-Host '  - PC port saves (0.MCD, 8.MCD, ... or the gamedata\save folder)  -> DuckStation / PSX cards'
    Write-Host '  - both at once                                                   -> merge one into the other'
    Finish 1
}

$pcFiles = New-Object System.Collections.Generic.List[string]
$psxFiles = New-Object System.Collections.Generic.List[string]
foreach ($p in $Paths) {
    if (Test-Path -LiteralPath $p -PathType Container) {
        foreach ($f in Get-ChildItem -LiteralPath $p -File) {
            if ([ShSaveCard.Converter]::IsPcCardFileName($f.Name)) { $pcFiles.Add($f.FullName) }
        }
    } elseif (Test-Path -LiteralPath $p -PathType Leaf) {
        $full = (Resolve-Path -LiteralPath $p).Path
        if ([ShSaveCard.Converter]::IsPcCardFileName($full)) { $pcFiles.Add($full) } else { $psxFiles.Add($full) }
    } else {
        Write-Host "Not found: $p"; Finish 1
    }
}
$pcFiles = @($pcFiles | Sort-Object { [int][IO.Path]::GetFileNameWithoutExtension($_) })

$pcCards = @{}; $psxCards = @{}
try {
    foreach ($f in $pcFiles) { $pcCards[$f] = Load-Card $f }
    foreach ($f in $psxFiles) { $psxCards[$f] = Load-Card $f }
} catch {
    Write-Host "Could not read ${f}:"; Write-Host "  $($_.Exception.InnerException.Message)$($_.Exception.Message)"; Finish 1
}

# Info: list what each card holds and stop.
if ($Info) {
    foreach ($f in @($pcFiles) + @($psxFiles)) {
        $c = if ($pcCards.ContainsKey($f)) { $pcCards[$f] } else { $psxCards[$f] }
        Write-Host "$f  ($($c.Files.Count) file(s), $($c.FreeBlocks) free block(s))"
        foreach ($w in $c.Warnings) { Write-Host "  ! $w" }
        foreach ($cf in $c.Files) {
            $r = [ShSaveCard.Region]::Usa; $i = 0
            if ([ShSaveCard.ShFile]::Parse($cf.Name, [ref]$r, [ref]$i)) {
                Write-Host ("  {0}  Silent Hill ({1}) FILE{2:00}" -f $cf.Name, [ShSaveCard.ShFile]::RegionLabel($r), ($i + 1))
                foreach ($s in [ShSaveCard.ShFile]::ReadSlots($cf.Data)) { Write-Host "    $s" }
            } else {
                Write-Host ("  {0}  (other game, {1} block(s))" -f $cf.Name, $cf.Blocks)
            }
        }
        Write-Host ''
    }
    Finish 0
}

if ($Mode -eq 'auto') {
    if ($pcFiles.Count -gt 0 -and $psxFiles.Count -eq 0) { $Mode = 'topsx' }
    elseif ($psxFiles.Count -gt 0 -and $pcFiles.Count -eq 0) { $Mode = 'topc' }
    elseif ($psxFiles.Count -eq 1) {
        Write-Host 'You gave both PC saves and a PSX memory card. What should happen?'
        Write-Host "  1) Add the PC saves into a copy of $([IO.Path]::GetFileName($psxFiles[0]))  (for DuckStation / real PS1)"
        Write-Host "  2) Add the saves on $([IO.Path]::GetFileName($psxFiles[0])) into a copy of the PC card $([IO.Path]::GetFileName($pcFiles[0]))"
        if (-not $interactive) { Write-Host 'Pass -Mode pc-into-psx or -Mode psx-into-pc.'; Finish 1 }
        $ans = Read-Host 'Choose 1 or 2'
        if ($ans -eq '1') { $Mode = 'pc-into-psx' } elseif ($ans -eq '2') { $Mode = 'psx-into-pc' } else { Finish 1 }
    } else {
        Write-Host 'Give either PSX cards, PC cards, or PC cards plus exactly one PSX card.'; Finish 1
    }
}

$usa = [ShSaveCard.Region]::Usa
$firstDir = Split-Path -Parent (@($pcFiles) + @($psxFiles))[0]

switch ($Mode) {
    'topc' {
        # PSX card k -> PC card slot k, in the order Memory Card 1, Memory Card 2, then multitap slots.
        $order = 0, 8, 1, 2, 3, 9, 10, 11
        if ($psxFiles.Count -gt $order.Count) { Write-Host 'At most 8 cards can be converted at once.'; Finish 1 }
        $out = New-OutDir $firstDir 'SH saves for PC'
        $written = 0
        for ($k = 0; $k -lt $psxFiles.Count; $k++) {
            $src = $psxFiles[$k]
            $res = [ShSaveCard.Converter]::Gather(@($psxCards[$src]), @([IO.Path]::GetFileName($src)), $usa, $false)
            Show-Log $res
            if ($res.Card.Files.Count -eq 0) { Write-Host "  (no Silent Hill saves on $([IO.Path]::GetFileName($src)))"; continue }
            $n = $order[$written]
            $dst = Join-Path $out "$n.MCD"
            [IO.File]::WriteAllBytes($dst, $res.Card.ToImage())
            Write-Host ("  => {0}  = {1} in game" -f $dst, [ShSaveCard.Converter]::PcCardLabel($n))
            Write-Host ''
            $written++
        }
        if ($written -eq 0) { Write-Host 'Nothing to convert.'; Finish 1 }
        Write-Host 'Copy the .MCD file(s) into the game''s gamedata\save folder.'
        Write-Host 'Back up that folder first: a same-named card there will be replaced.'
    }
    'topsx' {
        $reg = Ask-Region
        $out = New-OutDir $firstDir 'SH saves for PSX'
        $written = 0
        foreach ($src in $pcFiles) {
            $n = [int][IO.Path]::GetFileNameWithoutExtension($src)
            $res = [ShSaveCard.Converter]::Gather(@($pcCards[$src]), @([IO.Path]::GetFileName($src)), $reg, $true)
            Show-Log $res
            if ($res.Card.Files.Count -eq 0) { continue }
            $slot = if ($n -ge 8) { '2' } else { '1' }
            if (($n -band 3) -ne 0) { $slot += [char](65 + ($n -band 3)) }
            $game = switch ($reg) { ([ShSaveCard.Region]::Eur) { 'Silent Hill (Europe) (En,Fr,De,Es,It)' } ([ShSaveCard.Region]::Jpn) { 'Silent Hill (Japan)' } default { 'Silent Hill (USA)' } }
            $dst = Join-Path $out "${game}_$slot.mcd"
            [IO.File]::WriteAllBytes($dst, $res.Card.ToImage())
            Write-Host "  => $dst"
            Write-Host ''
            $written++
        }
        if ($written -eq 0) { Write-Host 'No Silent Hill saves found in the PC cards.'; Finish 1 }
        Write-Host 'DuckStation: put the .mcd in its memcards folder (per-game card mode) or open it with'
        Write-Host 'Tools > Memory Card Editor to copy saves into another card.'
        Write-Host 'Real PS1 (MemCard Pro / SD2PSX etc.): the .mcd is a raw card image; rename it to what your device expects (e.g. .mcr).'
    }
    'pc-into-psx' {
        $base = $psxFiles[0]
        $reg = if ($Region) { Region-Of $Region } else { Card-Region $psxCards[$base] }
        if ($null -eq $reg) { $reg = Ask-Region }
        $adds = @($pcFiles | ForEach-Object { $pcCards[$_] })
        $labels = @($pcFiles | ForEach-Object { [IO.Path]::GetFileName($_) })
        $res = [ShSaveCard.Converter]::Merge($psxCards[$base], [IO.Path]::GetFileName($base), $adds, $labels, $reg, $true)
        Show-Log $res
        $out = New-OutDir $firstDir 'SH saves for PSX'
        $dst = Join-Path $out ([IO.Path]::GetFileName($base))
        [IO.File]::WriteAllBytes($dst, $res.Card.ToImage())
        Write-Host "  => $dst  ($($res.Card.FreeBlocks) free block(s) left)"
        Write-Host ''
        Write-Host 'Replace the original card with this one (keep a backup of the original).'
    }
    'psx-into-pc' {
        $base = $pcFiles[0]
        $n = [int][IO.Path]::GetFileNameWithoutExtension($base)
        $adds = @($psxFiles | ForEach-Object { $psxCards[$_] })
        $labels = @($psxFiles | ForEach-Object { [IO.Path]::GetFileName($_) })
        $res = [ShSaveCard.Converter]::Merge($pcCards[$base], [IO.Path]::GetFileName($base), $adds, $labels, $usa, $false)
        Show-Log $res
        $out = New-OutDir $firstDir 'SH saves for PC'
        $dst = Join-Path $out "$n.MCD"
        [IO.File]::WriteAllBytes($dst, $res.Card.ToImage())
        Write-Host ("  => {0}  = {1} in game" -f $dst, [ShSaveCard.Converter]::PcCardLabel($n))
        Write-Host ''
        Write-Host 'Copy it into the game''s gamedata\save folder (back up the original first).'
    }
}
Finish 0
