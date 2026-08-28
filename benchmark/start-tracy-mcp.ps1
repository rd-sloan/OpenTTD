# Start the Tracy MCP server for Claude Code.
#
# The server is a Python sidecar shipped in the Tracy source tree. It needs the
# TracyServerBindings module built from that same tree at the same tag as our
# client, because it checks the wire protocol version before attaching to a
# running game. See docs/tracy-mcp.md for the build steps.
#
# The server refuses to start a second copy, so re-running this while one is up
# just reports the existing port and exits.

param(
    [string]$TracyRoot = 'C:\git\tracy',
    [int]$Port = 47380
)

$ErrorActionPreference = 'Stop'

$bindings = Join-Path $TracyRoot 'build\python\Release'
$venvPython = Join-Path $TracyRoot '.venv\Scripts\python.exe'
$script = Join-Path $TracyRoot 'extra\mcp\tracy_mcp.py'
$captures = Join-Path $PSScriptRoot 'manual-traces'

foreach ($path in @($bindings, $venvPython, $script)) {
    if (-not (Test-Path $path)) {
        throw "Missing $path. Work through the build steps in docs/tracy-mcp.md first."
    }
}

$env:PYTHONPATH = $bindings
$env:TRACY_CAPTURES_DIR = $captures
$env:TRACY_MCP_PORT = "$Port"

Write-Host "Captures:  $captures"
Write-Host "Bindings:  $bindings"
Write-Host ''

& $venvPython $script
