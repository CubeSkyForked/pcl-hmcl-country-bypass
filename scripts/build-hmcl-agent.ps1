param([string]$JavaHome = '')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$outputDirectory = Join-Path $projectRoot 'build\hmcl-agent'
$classesDirectory = Join-Path $outputDirectory ('classes-' + [Guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $classesDirectory -Force)
$compiler = if ($JavaHome) { Join-Path $JavaHome 'bin\javac.exe' } else { (Get-Command javac -ErrorAction Stop).Source }
$sources = @(Get-ChildItem -LiteralPath (Join-Path $projectRoot 'java') -Filter 'Hmcl*.java' | Sort-Object Name | Select-Object -ExpandProperty FullName)
& $compiler --release 17 -encoding UTF-8 -g -d $classesDirectory @sources
if ($LASTEXITCODE -ne 0) { throw "Java agent compilation failed: $LASTEXITCODE" }
& python (Join-Path $PSScriptRoot 'embed-hmcl-agent.py') $classesDirectory `
    (Join-Path $outputDirectory 'HKRegion.HMCL.Agent.jar') (Join-Path $projectRoot 'src\hmcl_agent_payload.h')
if ($LASTEXITCODE -ne 0) { throw "Embedding the Java agent failed: $LASTEXITCODE" }
