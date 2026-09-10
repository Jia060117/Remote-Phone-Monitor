$ErrorActionPreference = "Stop"
$releaseRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$serverRoot = Join-Path $releaseRoot "server"

if (-not (Test-Path -LiteralPath (Join-Path $serverRoot "server.json")) -or
    -not (Test-Path -LiteralPath (Join-Path $serverRoot "tokens.json"))) {
    throw "Run 'python initialize.py --server-url https://your-server.example' first."
}

Set-Location -LiteralPath $serverRoot
if (-not (Test-Path -LiteralPath ".venv\Scripts\python.exe")) {
    python -m venv .venv
}
& ".venv\Scripts\python.exe" -m pip install -r requirements.txt
& ".venv\Scripts\python.exe" -m uvicorn main:app --host 0.0.0.0 --port 8000
