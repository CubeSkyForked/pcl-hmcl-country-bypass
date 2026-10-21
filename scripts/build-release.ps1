param(
    [string]$JavaHome = '',
    [string]$Generator = 'Visual Studio 17 2022',
    [string]$VisualStudioInstance = '',
    [ValidateRange(1, 64)][int]$Parallel = 4
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$cmake = (Get-Command cmake -ErrorAction Stop).Source
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
$python = (Get-Command python -ErrorAction Stop).Source

# Rebuild from checked-in source once so both architectures embed the same payloads.
& (Join-Path $PSScriptRoot 'build-managed.ps1')
& (Join-Path $PSScriptRoot 'build-hmcl-agent.ps1') -JavaHome $JavaHome

$analysisArguments = @((Join-Path $root 'tests\hmcl\run-analysis-tests.py'))
if ($JavaHome) { $analysisArguments += @('--java-home', $JavaHome) }
& $python @analysisArguments
if ($LASTEXITCODE -ne 0) { throw "Java class analysis checks failed: $LASTEXITCODE" }

foreach ($architecture in @('x64', 'Win32')) {
    $directory = Join-Path $root ('build\msvc-' + $architecture)
    $configure = @('-S', $root, '-B', $directory, '-G', $Generator, '-A', $architecture, '-DBUILD_TESTING=ON')
    if ($VisualStudioInstance) { $configure += '-DCMAKE_GENERATOR_INSTANCE=' + $VisualStudioInstance }
    & $cmake @configure
    if ($LASTEXITCODE -ne 0) { throw "Configuration failed for $architecture`: $LASTEXITCODE" }
    & $cmake --build $directory --config Release --parallel $Parallel
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed for $architecture`: $LASTEXITCODE" }
    & $ctest --test-dir $directory -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "Tests failed for $architecture`: $LASTEXITCODE" }
}

& $python (Join-Path $PSScriptRoot 'package-release.py')
if ($LASTEXITCODE -ne 0) { throw "Release packaging failed: $LASTEXITCODE" }
Write-Host 'Release files: dist\winmm.dll (x64), dist\version.dll (x86).'
