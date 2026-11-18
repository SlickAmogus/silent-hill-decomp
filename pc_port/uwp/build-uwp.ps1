# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
    Builds the UWP package (Xbox One / Series dev mode, Windows 10+).

.DESCRIPTION
    Configures and builds pc_port with uwp/toolchain-uwp-clang.cmake, lays the
    package out in <BuildDir>\appx, then packs and signs it.

    Needs LLVM (clang, lld-link, llvm-objcopy), Ninja, CMake, Python 3 with
    Pillow, the VS 2022 C++ toolset, a Windows 10 SDK, and UWP builds of the
    dependencies:

        vcpkg install sdl2 openal-soft libjpeg-turbo angle --triplet x64-uwp

.PARAMETER Deps
    The vcpkg installed\x64-uwp directory.

.PARAMETER Register
    Also register the loose layout with this Windows account (needs Developer
    Mode), which is the quickest way to run a build without installing it.
#>
param(
    [Parameter(Mandatory = $true)][string]$Deps,
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build-uwp'),
    [string]$Version = '0.1.0.0',
    [string]$Publisher = 'CN=SilentHillDecomp',
    [string]$Pfx = (Join-Path $env:LOCALAPPDATA 'SilentHillUWP\signing.pfx'),
    [switch]$Register
)

$ErrorActionPreference = 'Stop'
$pcPort = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Deps = (Resolve-Path $Deps).Path

function Invoke-Tool {
    param([string]$Exe, [string[]]$Arguments)
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed ($LASTEXITCODE)" }
}

function Find-SdkTool {
    param([string]$Name)
    $hit = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.*\x64\$Name" -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $hit) { throw "$Name not found in the Windows SDK" }
    $hit.FullName
}

if (-not (Test-Path (Join-Path $BuildDir 'build.ninja'))) {
    Invoke-Tool cmake @(
        '-S', $pcPort, '-B', $BuildDir, '-G', 'Ninja',
        "-DCMAKE_TOOLCHAIN_FILE=$PSScriptRoot\toolchain-uwp-clang.cmake",
        "-DSH_UWP_DEPS=$Deps")
}
Invoke-Tool cmake @('--build', $BuildDir)

$layout = Join-Path $BuildDir 'appx'
if (Test-Path $layout) { Remove-Item $layout -Recurse -Force }
New-Item -ItemType Directory -Force $layout, (Join-Path $layout 'data') | Out-Null

Copy-Item (Join-Path $BuildDir 'SilentHillUWP.exe') $layout
Copy-Item (Join-Path $Deps 'bin\*.dll') $layout

# Staged into LocalState on first run (uwp_main.cpp); the install folder itself
# is read-only, and every path the port opens is relative to LocalState. Named
# data, not assets: the package's own Assets folder holds the tile images.
Copy-Item (Join-Path $pcPort 'assets\*') (Join-Path $layout 'data') -Recurse
Copy-Item (Join-Path $pcPort 'config.cfg') (Join-Path $layout 'data')

Invoke-Tool python @((Join-Path $PSScriptRoot 'make_logos.py'),
    (Join-Path $pcPort 'res\app_icon.ico'), (Join-Path $layout 'Assets'))

(Get-Content (Join-Path $PSScriptRoot 'AppxManifest.xml.in') -Raw).
    Replace('@SH_UWP_PUBLISHER@', $Publisher).
    Replace('@SH_UWP_VERSION@', $Version) |
    Set-Content (Join-Path $layout 'AppxManifest.xml') -Encoding utf8

# Dev-mode consoles and sideloading PCs only need the package signed by a
# certificate whose subject matches the manifest's Publisher; it does not have
# to chain to anything. One is made on first use and kept outside the repo.
if (-not (Test-Path $Pfx)) {
    New-Item -ItemType Directory -Force (Split-Path $Pfx) | Out-Null
    $cert = New-SelfSignedCertificate -Type Custom -Subject $Publisher `
        -KeyUsage DigitalSignature -FriendlyName 'Silent Hill UWP test signing' `
        -CertStoreLocation 'Cert:\CurrentUser\My' -NotAfter (Get-Date).AddYears(10) `
        -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
    $empty = New-Object System.Security.SecureString
    Export-PfxCertificate -Cert $cert -FilePath $Pfx -Password $empty | Out-Null
    Export-Certificate -Cert $cert -FilePath ([IO.Path]::ChangeExtension($Pfx, '.cer')) | Out-Null
}

$appx = Join-Path $BuildDir "SilentHill_UWP_${Version}_x64.appx"
Invoke-Tool (Find-SdkTool 'makeappx.exe') @('pack', '/o', '/d', $layout, '/p', $appx)
Invoke-Tool (Find-SdkTool 'signtool.exe') @('sign', '/fd', 'SHA256', '/f', $Pfx, $appx)
Copy-Item ([IO.Path]::ChangeExtension($Pfx, '.cer')) ([IO.Path]::ChangeExtension($appx, '.cer'))
Write-Host "Package: $appx"

if ($Register) {
    Add-AppxPackage -Register (Join-Path $layout 'AppxManifest.xml')
    Write-Host 'Registered the loose layout for this account.'
}
