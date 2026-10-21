param()
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$outputDirectory = Join-Path $projectRoot 'build\managed'
[void](New-Item -ItemType Directory -Path $outputDirectory -Force)
$frameworkDirectory = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319'
if (-not (Test-Path -LiteralPath (Join-Path $frameworkDirectory 'csc.exe'))) {
    $frameworkDirectory = Join-Path $env:WINDIR 'Microsoft.NET\Framework\v4.0.30319'
}
$compiler = Join-Path $frameworkDirectory 'csc.exe'
$assembly = Join-Path $outputDirectory 'HKRegion.Managed.dll'
& $compiler /nologo /target:library /optimize+ /platform:anycpu "/out:$assembly" `
    "/reference:$(Join-Path $frameworkDirectory 'WPF\WindowsBase.dll')" `
    "/reference:$(Join-Path $frameworkDirectory 'WPF\PresentationFramework.dll')" `
    "/reference:$(Join-Path $frameworkDirectory 'System.Xaml.dll')" `
    (Join-Path $projectRoot 'managed\PclDownloadRestore.cs')
if ($LASTEXITCODE -ne 0) { throw "Managed helper compilation failed: $LASTEXITCODE" }
& python (Join-Path $PSScriptRoot 'embed-managed.py') $assembly (Join-Path $projectRoot 'src\managed_payload.h')
if ($LASTEXITCODE -ne 0) { throw "Embedding the managed helper failed: $LASTEXITCODE" }
