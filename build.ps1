<#
.SYNOPSIS
  Configure, build, test and deploy TanzTrans (MSVC + Ninja + Qt).
.EXAMPLE
  ./build.ps1                 # release build + tests + windeployqt (runs from build\release)
  ./build.ps1 -Preset debug
  ./build.ps1 -UpdateTranslations
  ./build.ps1 -Package        # also dist\TanzTrans-<version>.zip for users (Qt + VC++ runtime included)
#>
param(
    [ValidateSet('debug', 'release')] [string] $Preset = 'release',
    [switch] $UpdateTranslations,
    [switch] $NoTests,
    [switch] $NoDeploy,
    [switch] $Package
)
$ErrorActionPreference = 'Stop'
if (-not $env:QTDIR) { $env:QTDIR = 'C:\Qt\6.11.2\msvc2022_64' }

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
}

Push-Location $PSScriptRoot
try {
    cmake --preset $Preset
    if ($LASTEXITCODE) { throw 'configure failed' }
    if ($UpdateTranslations) {
        cmake --build --preset $Preset --target update_translations
        if ($LASTEXITCODE) { throw 'lupdate failed' }
    }
    cmake --build --preset $Preset
    if ($LASTEXITCODE) { throw 'build failed' }
    $bin = Join-Path $PSScriptRoot "build\$Preset"
    $env:PATH = "$env:QTDIR\bin;$env:PATH"
    if (-not $NoTests) {
        ctest --preset $Preset
        if ($LASTEXITCODE) { throw 'tests failed' }
    }
    # Widgets only (no Qt Quick/RHI shaders), SQLite only, the two UI languages only.
    $deploy = @('--no-compiler-runtime', '--no-system-d3d-compiler', '--no-system-dxc-compiler', '--no-opengl-sw',
                '--exclude-plugins', 'qsqlibase,qsqlmimer,qsqloci,qsqlodbc,qsqlpsql', '--translations', 'en,de')
    if (-not $NoDeploy) {
        & "$env:QTDIR\bin\windeployqt.exe" @deploy (Join-Path $bin 'TanzTrans.exe') | Out-Null
        if ($LASTEXITCODE) { throw 'windeployqt failed' }
        "Deployed: $bin\TanzTrans.exe"
    }
    if ($Package) {
        if ($Preset -ne 'release') { throw '-Package needs the release preset' }
        $version = [regex]::Match((Get-Content -Raw CMakeLists.txt), 'project\(TanzTrans VERSION ([0-9.]+)').Groups[1].Value
        $dist = Join-Path $PSScriptRoot 'dist\TanzTrans'
        Remove-Item -Recurse -Force $dist -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force $dist | Out-Null
        Copy-Item (Join-Path $bin 'TanzTrans.exe'), README.md $dist
        & "$env:QTDIR\bin\windeployqt.exe" @deploy (Join-Path $dist 'TanzTrans.exe') | Out-Null
        if ($LASTEXITCODE) { throw 'windeployqt failed' }
        # Visual C++ runtime next to the exe ("app-local"), so users need not install anything.
        $redist = $env:VCToolsRedistDir
        if (-not $redist) {
            $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
            $redist = (Get-ChildItem "$vs\VC\Redist\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1).FullName
        }
        $crt = Get-ChildItem (Join-Path $redist 'x64') -Directory -Filter 'Microsoft.VC*.CRT' | Select-Object -First 1
        if (-not $crt) { throw "VC++ runtime not found in $redist" }
        Copy-Item "$($crt.FullName)\*.dll" $dist
        $zip = Join-Path $PSScriptRoot "dist\TanzTrans-$version.zip"
        Compress-Archive -Path $dist -DestinationPath $zip -Force
        "Package: $zip"
    }
}
finally { Pop-Location }
