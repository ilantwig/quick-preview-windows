param([string]$ToolsDirectory = (Join-Path $PSScriptRoot '..\.tools'))
$ErrorActionPreference = 'Stop'
$version = '20261117'
$folder = "llvm-mingw-$version-ucrt-x86_64"
$expectedHash = '4d002e330a0f703625aa5ccbc37e96f8f824413e163bfc7336774aabd2c3a412'
New-Item -ItemType Directory -Path $ToolsDirectory -Force | Out-Null
$ToolsDirectory = (Resolve-Path -LiteralPath $ToolsDirectory).Path
$archive = Join-Path $ToolsDirectory "$folder.zip"
$compiler = Join-Path $ToolsDirectory "$folder\bin\x86_64-w64-mingw32-clang++.exe"
if (Test-Path -LiteralPath $compiler) {
    Write-Host "Using existing toolchain: $compiler"
    return
}
if (-not (Test-Path -LiteralPath $archive)) {
    $download = "$archive.download"
    Invoke-WebRequest -Uri "https://github.com/mstorsjo/llvm-mingw/releases/download/$version/$folder.zip" -OutFile $download
    if ((Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedHash) {
        throw 'Toolchain download checksum mismatch. The unverified file was not extracted.'
    }
    Move-Item -LiteralPath $download -Destination $archive -Force
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedHash) {
    throw 'Cached toolchain checksum mismatch. Download the official archive again.'
}
Expand-Archive -LiteralPath $archive -DestinationPath $ToolsDirectory -Force
if (-not (Test-Path -LiteralPath $compiler)) { throw 'The extracted toolchain is incomplete.' }
Write-Host "Verified toolchain ready: $compiler"

