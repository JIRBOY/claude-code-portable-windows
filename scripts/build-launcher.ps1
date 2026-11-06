param(
    [string]$Root = '',
    [string]$ToolchainRoot = ''
)
$ErrorActionPreference = 'Stop'

if (-not $Root) { $Root = (Get-Location).Path }
$Root = (Resolve-Path -LiteralPath $Root).Path

if (-not $ToolchainRoot) {
    $ToolchainRoot = Join-Path $env:TEMP 'llvm-mingw-20261104\extracted\llvm-mingw-20261104-ucrt-x86_64'
}
$clang = Join-Path $ToolchainRoot 'bin\x86_64-w64-mingw32-clang.exe'
if (-not (Test-Path -LiteralPath $clang -PathType Leaf)) {
    throw "LLVM-MinGW compiler not found: $clang. Run scripts/get-toolchain.ps1 first."
}

$src = Join-Path $Root 'src'
if (-not (Test-Path -LiteralPath (Join-Path $src 'scc.c') -PathType Leaf)) {
    throw "Source tree not found: $src. Copy assets/src into the portable root first."
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$snapshot = Join-Path $Root ".snapshots\launcher-$stamp"
New-Item -ItemType Directory -Path $snapshot -Force | Out-Null

function Backup-GeneratedFile([string]$Path) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        $destination = Join-Path $snapshot ([IO.Path]::GetFileName($Path))
        Move-Item -LiteralPath $Path -Destination $destination -ErrorAction Stop
    }
}

$runtimeBin = Join-Path $Root 'runtime\bin'
$mockBin = Join-Path $Root 'tests\mock\bin'
New-Item -ItemType Directory -Path $runtimeBin -Force | Out-Null
New-Item -ItemType Directory -Path $mockBin -Force | Out-Null

$scc = Join-Path $Root 'scc.exe'
$mock = Join-Path $mockBin 'claude.exe'
$shim = Join-Path $runtimeBin 'tool_shim.exe'
$shimNames = @('pip.exe', 'pytest.exe', 'playwright.exe', 'markitdown.exe')

Backup-GeneratedFile $scc
Backup-GeneratedFile $mock
Backup-GeneratedFile $shim
foreach ($name in $shimNames) { Backup-GeneratedFile (Join-Path $runtimeBin $name) }

$common = @(
    '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
    '-static', '-municode', '-DUNICODE', '-D_UNICODE', '-D_WIN32_WINNT=0x0A00'
)

& $clang @common '-o' $scc (Join-Path $src 'scc.c') '-lshell32'
if ($LASTEXITCODE -ne 0) { throw "scc.c compilation failed: $LASTEXITCODE" }

if (Test-Path -LiteralPath (Join-Path $src 'mock_claude.c')) {
    & $clang @common '-o' $mock (Join-Path $src 'mock_claude.c') '-lshell32'
    if ($LASTEXITCODE -ne 0) { throw "mock_claude.c compilation failed: $LASTEXITCODE" }
}

if (Test-Path -LiteralPath (Join-Path $src 'tool_shim.c')) {
    & $clang @common '-o' $shim (Join-Path $src 'tool_shim.c')
    if ($LASTEXITCODE -ne 0) { throw "tool_shim.c compilation failed: $LASTEXITCODE" }
    foreach ($name in $shimNames) {
        Copy-Item -LiteralPath $shim -Destination (Join-Path $runtimeBin $name) -ErrorAction Stop
    }
}

Copy-Item -LiteralPath $scc -Destination (Join-Path $mockBin 'scc.exe') -ErrorAction Stop

$report = [ordered]@{
    GeneratedAt = (Get-Date).ToUniversalTime().ToString('o')
    Root = $Root
    Compiler = (& $clang --version | Select-Object -First 1)
    Scc = [ordered]@{
        Path = 'scc.exe'
        SHA256 = (Get-FileHash -LiteralPath $scc -Algorithm SHA256).Hash
        Bytes = (Get-Item -LiteralPath $scc).Length
    }
}
if (Test-Path -LiteralPath $mock) {
    $report.Mock = [ordered]@{
        Path = 'tests/mock/bin/claude.exe'
        SHA256 = (Get-FileHash -LiteralPath $mock -Algorithm SHA256).Hash
    }
}
$report | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $Root 'build-launcher-report.json') -Encoding UTF8
$report | ConvertTo-Json -Depth 4
