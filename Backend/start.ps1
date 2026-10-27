param([string]$PythonExecutable = 'python')
$ErrorActionPreference = 'Stop'
& $PythonExecutable (Join-Path $PSScriptRoot 'server.py') serve
exit $LASTEXITCODE
