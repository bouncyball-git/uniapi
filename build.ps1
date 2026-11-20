<#
.SYNOPSIS
    Builds uniapi on Windows, and cleans, tests, installs or uninstalls it.

.DESCRIPTION
    Runs the actions given, in order; with none, it builds. Needs Visual Studio 2022 or Build
    Tools 2022 with "Desktop development with C++": the script finds it and sets up its build
    environment, so any PowerShell window works (not only the VS developer prompt).

      clean           deletes the build folders: "build" and every "build-*" holding a CMake
                      build. With -BuildDir: only that folder
      build           configures (if needed) and builds everything: library, examples, tests
      test            runs the tests (ctest; builds first)
      install         installs the example apps to <prefix>\bin (builds first)
      uninstall       removes what install put in <prefix>
      clean-profiles  deletes the web engine profiles (caches, downloaded components) of the
                      programs this project builds: %LOCALAPPDATA%\<program>, also under old
                      program names, and the old <program>.exe.WebView2 folders in the build
                      folder. It lists them and asks first (-Yes doesn't ask). A folder holding
                      anything but a WebView2 profile is skipped: it may be another app's.
                      Each program makes a fresh profile when it next starts; the first quick
                      runs on a fresh profile are a few seconds slower.
      help            shows this help (also -Help)

    install keeps a list of the files it installed in <prefix>\uniapi-install-manifest.txt;
    uninstall removes exactly those, then the folders left empty. So uninstall works after
    clean too, given the same -Prefix.

.PARAMETER Actions
    What to do, in order: clean, build, test, install, uninstall, clean-profiles, help.
    Default: build.

.PARAMETER Prefix
    Where install puts the apps and uninstall removes them. Default: the "install" folder in
    the repository (no administrator rights needed). A folder like C:\Program Files\uniapi
    needs an elevated PowerShell.

.PARAMETER Config
    Release (default) or Debug.

.PARAMETER BuildDir
    The build folder. Default: "build" in the repository.

.PARAMETER Yes
    clean-profiles deletes without asking.

.PARAMETER Help
    Shows this help.

.EXAMPLE
    .\build.ps1
    .\build.ps1 clean build test
    .\build.ps1 install -Prefix C:\Apps\uniapi
    .\build.ps1 uninstall -Prefix C:\Apps\uniapi
    .\build.ps1 clean-profiles
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [ValidateSet('clean', 'build', 'test', 'install', 'uninstall', 'clean-profiles', 'help')]
    [string[]] $Actions = @('build'),
    [string] $Prefix,
    [ValidateSet('Release', 'Debug')]
    [string] $Config = 'Release',
    [string] $BuildDir,
    [switch] $Yes,
    [switch] $Help
)

if ($Help -or $Actions -contains 'help') {
    Get-Help $PSCommandPath -Detailed
    return
}

$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot
$BuildDirNamed = [bool]$BuildDir  # -BuildDir given: clean deletes only that folder
if (-not $BuildDir) { $BuildDir = Join-Path $Root 'build' }
if (-not $Prefix) { $Prefix = Join-Path $Root 'install' }
# Relative paths are relative to the current PowerShell folder.
$BuildDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BuildDir)
$Prefix = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Prefix).TrimEnd('\')
$ManifestName = 'uniapi-install-manifest.txt'
# The programs this project builds, whose profiles clean-profiles removes.
$Programs = 'hello', 'hello_c', 'uniapi_stream', 'uniapi_notify', 'uniapi_tray', 'uniapi_logo', 'uniapi_test',
            'uniapi_test_c', 'uniapi_windows_test', 'uniapi_lifecycle_test', 'uniapi_desktop_test', 'uniapi_placement_test', 'uniapi_tray_test', 'uniapi_strict_test', 'uniapi_modal_test',
            'uniapi_checker', 'uniapi_checker_c'

function Step([string] $text) { Write-Host "==> $text" -ForegroundColor Cyan }

# Runs a native command and stops the script if it fails.
function Run {
    & $args[0] @($args | Select-Object -Skip 1)
    if ($LASTEXITCODE -ne 0) { throw "failed (exit code $LASTEXITCODE): $($args -join ' ')" }
}

# Brings the Visual Studio C++ build environment (compiler, Windows SDK, CMake, Ninja) into
# this PowerShell session, unless it is already there.
function Use-VisualStudio {
    if ((Get-Command cl.exe -ErrorAction SilentlyContinue) -and (Get-Command cmake -ErrorAction SilentlyContinue) -and
        (Get-Command ninja -ErrorAction SilentlyContinue)) { return }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw 'Visual Studio 2022 (or Build Tools) not found: vswhere.exe is missing.' }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'No Visual Studio with the C++ tools found ("Desktop development with C++").' }
    $arch = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { '64' }
    $vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars$arch.bat"
    Step "Visual Studio environment ($vs)"
    # Run vcvars in cmd and copy the environment it leaves into this session.
    $lines = cmd /c "`"$vcvars`" >nul 2>&1 && set"
    if ($LASTEXITCODE -ne 0) { throw "vcvars failed: $vcvars" }
    foreach ($line in $lines) {
        $eq = $line.IndexOf('=')
        if ($eq -gt 0) { Set-Item -Path "env:$($line.Substring(0, $eq))" -Value $line.Substring($eq + 1) }
    }
}

# The folder named (-BuildDir), else every build folder in the repository: "build", and each
# "build-*" that holds a CMake build (CMakeCache.txt), so a folder that only happens to have
# such a name stays.
function Invoke-Clean {
    if ($script:BuildDirNamed) {
        $dirs = @($BuildDir | Where-Object { Test-Path $_ })
    } else {
        $dirs = @(Get-ChildItem -Path $Root -Directory | Where-Object {
            $_.Name -eq 'build' -or ($_.Name -like 'build-*' -and (Test-Path (Join-Path $_.FullName 'CMakeCache.txt')))
        } | ForEach-Object { $_.FullName })
    }
    if ($dirs.Count -eq 0) {
        Step 'clean: no build folder to delete'
        return
    }
    foreach ($dir in $dirs) {
        Step "clean: deleting $dir"
        Remove-Item -Recurse -Force $dir
    }
}

function Invoke-Build {
    Use-VisualStudio
    Step "configure ($Config) in $BuildDir"
    Run cmake -S $Root -B $BuildDir -G Ninja "-DCMAKE_BUILD_TYPE=$Config"
    Step 'build'
    Run cmake --build $BuildDir
}

function Invoke-Test {
    Step 'test'
    Run ctest --test-dir $BuildDir --output-on-failure
}

function Invoke-Install {
    Step "install to $Prefix"
    Run cmake --install $BuildDir --prefix $Prefix
    # Keep CMake's list of installed files with the install, for uninstall.
    $manifest = Join-Path $BuildDir 'install_manifest.txt'
    if (-not (Test-Path $manifest)) { throw "CMake wrote no $manifest" }
    Copy-Item $manifest (Join-Path $Prefix $ManifestName) -Force
    Get-Content $manifest | ForEach-Object { "    $_" }
}

function Invoke-Uninstall {
    $manifest = Join-Path $Prefix $ManifestName
    if (-not (Test-Path $manifest)) {
        Step "uninstall: nothing installed in $Prefix (no $ManifestName)"
        return
    }
    Step "uninstall from $Prefix"
    $folders = @{}
    foreach ($file in Get-Content $manifest) {
        if (-not $file.Trim()) { continue }
        $path = [IO.Path]::GetFullPath($file)
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            Remove-Item -LiteralPath $path -Force
            Write-Host "    removed $path"
        }
        $folders[(Split-Path $path -Parent)] = $true
    }
    Remove-Item -LiteralPath $manifest -Force
    $folders[$Prefix] = $true
    # Remove the folders the install created, if now empty: deepest first, up to the prefix.
    $all = @{}
    foreach ($folder in $folders.Keys) {
        $f = $folder
        while ($f -and $f.StartsWith($Prefix, [StringComparison]::OrdinalIgnoreCase)) {
            $all[$f] = $true
            $f = Split-Path $f -Parent
        }
    }
    foreach ($folder in $all.Keys | Sort-Object Length -Descending) {
        if ((Test-Path -LiteralPath $folder) -and -not (Get-ChildItem -LiteralPath $folder -Force)) {
            Remove-Item -LiteralPath $folder -Force
            Write-Host "    removed folder $folder"
        }
    }
}

function Invoke-CleanProfiles {
    $found = @()
    $candidates = @($Programs | ForEach-Object { Join-Path $env:LOCALAPPDATA $_ })
    # The old place, next to the programs (WebView2's default until 2026-11-03).
    if (Test-Path -LiteralPath $BuildDir) {
        $candidates += @(Get-ChildItem -LiteralPath $BuildDir -Directory -Filter '*.exe.WebView2' | ForEach-Object FullName)
    }
    foreach ($dir in $candidates) {
        if (-not (Test-Path -LiteralPath $dir)) { continue }
        # A WebView2 profile folder holds only EBWebView; anything else may be another app's.
        $other = @(Get-ChildItem -LiteralPath $dir -Force | Where-Object Name -ne 'EBWebView')
        if ($other.Count -or -not (Test-Path -LiteralPath (Join-Path $dir 'EBWebView'))) {
            Write-Host "    skipped $dir (doesn't look like a WebView2 profile)"
            continue
        }
        $found += $dir
    }
    if (-not $found.Count) {
        Step 'clean-profiles: no profiles found'
        return
    }
    Step 'clean-profiles'
    foreach ($dir in $found) {
        $mb = (Get-ChildItem -LiteralPath $dir -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum / 1MB
        Write-Host ('    {0}  ({1:N1} MB)' -f $dir, $mb)
    }
    if (-not $Yes) {
        $answer = Read-Host 'Delete these? [y/N]'
        if ($answer -notmatch '^(y|yes)$') {
            Write-Host '    nothing deleted'
            return
        }
    }
    foreach ($dir in $found) {
        try {
            Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction Stop
            Write-Host "    removed $dir"
        } catch {
            Write-Host "    could not remove $dir (is the program still running?): $($_.Exception.Message)" -ForegroundColor Yellow
        }
    }
}

$built = $false
foreach ($action in $Actions) {
    switch ($action) {
        'clean' { Invoke-Clean; $built = $false }
        'build' { Invoke-Build; $built = $true }
        'test' { if (-not $built) { Invoke-Build; $built = $true }; Invoke-Test }
        'install' { if (-not $built) { Invoke-Build; $built = $true }; Invoke-Install }
        'uninstall' { Invoke-Uninstall }
        'clean-profiles' { Invoke-CleanProfiles }
    }
}
Step 'done'
