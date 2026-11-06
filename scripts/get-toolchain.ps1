param(
    [string]$WorkRoot = '',
    [string]$Version = '20261104',
    [string]$ExpectedSha256 = '552191b5754c49a5a20723276a6fc56541ea1922c4c50b7b669022333fc1b40f'
)
$ErrorActionPreference = 'Stop'

if (-not $WorkRoot) {
    $WorkRoot = Join-Path $env:TEMP "llvm-mingw-$Version"
}

$archiveName = "llvm-mingw-$Version-ucrt-x86_64.zip"
$url = "https://github.com/mstorsjo/llvm-mingw/releases/download/$Version/$archiveName"
$compiler = Join-Path $WorkRoot "extracted\llvm-mingw-$Version-ucrt-x86_64\bin\x86_64-w64-mingw32-clang.exe"

if (Test-Path -LiteralPath $compiler -PathType Leaf) {
    [pscustomobject]@{ Status = 'already-present'; Compiler = $compiler } | ConvertTo-Json
    exit 0
}
if (Test-Path -LiteralPath $WorkRoot) {
    throw "Toolchain directory exists but compiler is missing; inspect instead of overwriting: $WorkRoot"
}

$parent = Split-Path -Parent $WorkRoot
if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
    throw "Parent directory does not exist: $parent"
}
New-Item -ItemType Directory -Path $WorkRoot | Out-Null
$archive = Join-Path $WorkRoot $archiveName
curl.exe -fL --retry 2 --max-time 300 -o $archive $url
if ($LASTEXITCODE -ne 0) { throw "Download failed: $LASTEXITCODE" }

$actualSha256 = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualSha256 -ne $ExpectedSha256.ToLowerInvariant()) {
    throw "SHA256 mismatch: expected $ExpectedSha256, got $actualSha256"
}

$extract = Join-Path $WorkRoot 'extracted'
New-Item -ItemType Directory -Path $extract | Out-Null
Expand-Archive -LiteralPath $archive -DestinationPath $extract -ErrorAction Stop
if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) {
    throw "Compiler missing after extraction: $compiler"
}

[pscustomobject]@{
    Status = 'downloaded'
    Version = $Version
    Url = $url
    SHA256 = $actualSha256
    Compiler = $compiler
} | ConvertTo-Json
