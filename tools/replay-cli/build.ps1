[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$EngineSource,
    [Parameter(Mandatory)][string]$ZlibSource,
    [string]$VsDevCmd
)

$ErrorActionPreference = 'Stop'
$project = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).ProviderPath
$engine = (Resolve-Path -LiteralPath $EngineSource).ProviderPath
$zlib = (Resolve-Path -LiteralPath $ZlibSource).ProviderPath
foreach ($patchName in @('classic-replay-map-triggers.patch', 'remastered-replay-map-triggers.patch')) {
    $patch = Join-Path $project "build/patches/$patchName"
    & git -C $engine apply --reverse --check $patch
    if ($LASTEXITCODE -ne 0) {
        throw "Use the OpenBW source tree with $patchName already applied."
    }
}
if (!(Test-Path -LiteralPath (Join-Path $engine 'engines/remastered/remastered_replay.h'))) {
    throw 'Use the Dan Gant OpenBW revision documented in the replay CLI README.'
}

if (!$VsDevCmd) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $VsDevCmd = & $vswhere -latest -prerelease -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'Common7\Tools\VsDevCmd.bat' |
        Select-Object -First 1
}
if (!$VsDevCmd -or !(Test-Path -LiteralPath $VsDevCmd -PathType Leaf)) {
    throw 'Visual Studio C++ tools were not found. Pass -VsDevCmd with the path to VsDevCmd.bat.'
}

$output = Join-Path $project 'agent/output/stl-rep-cli'
$temp = Join-Path $project 'agent/temp/stl-rep-cli'
[IO.Directory]::CreateDirectory($output) | Out-Null
[IO.Directory]::CreateDirectory($temp) | Out-Null

$compileScript = Join-Path $temp 'compile-stl-rep.cmd'
$source = Join-Path $PSScriptRoot 'engine.cpp'
$compiler = 'cl /nologo /EHsc /std:c++17 /bigobj /utf-8 /O2 /D_CRT_SECURE_NO_WARNINGS'
$lines = @(
    '@echo off'
    "call `"$VsDevCmd`" -arch=x64 -host_arch=x64 -no_logo"
    'if errorlevel 1 exit /b 1'
)

$zlibObjects = foreach ($name in @('adler32', 'crc32', 'inffast', 'inflate', 'inftrees', 'uncompr', 'zutil')) {
    $zlibFile = Join-Path $zlib "$name.c"
    if (!(Test-Path -LiteralPath $zlibFile -PathType Leaf)) { throw "Missing zlib source: $zlibFile" }
    Join-Path $temp "$name.obj"
}
foreach ($object in $zlibObjects) {
    $name = [IO.Path]::GetFileNameWithoutExtension($object)
    $zlibFile = Join-Path $zlib "$name.c"
    $lines += "cl /nologo /TC /c /O2 /D_CRT_SECURE_NO_WARNINGS `"$zlibFile`" /Fo:`"$object`""
    $lines += 'if errorlevel 1 exit /b 1'
}

$binaries = foreach ($kind in @('classic', 'remastered')) {
    $include = Join-Path $engine "engines/$kind"
    $object = Join-Path $temp "engine-$kind.obj"
    $suffix = ''
    $extra = ''
    if ($kind -eq 'remastered') {
        $suffix = '-remastered'
        $linkObjects = ($zlibObjects | ForEach-Object { '"' + $_ + '"' }) -join ' '
        $extra = "/DOPENBW_REMASTERED_RUNTIME /I`"$zlib`" $linkObjects"
    }
    $binary = Join-Path $output "stl-rep-engine$suffix.exe"
    $lines += "$compiler /I`"$include`" /I`"$engine`" $extra `"$source`" /Fo:`"$object`" /Fe:`"$binary`""
    $lines += 'if errorlevel 1 exit /b 1'
    $binary
}
[IO.File]::WriteAllLines($compileScript, $lines)
& $compileScript
if ($LASTEXITCODE -ne 0) { throw "Compiler exited with code $LASTEXITCODE." }
Get-Item -LiteralPath $binaries

$parserBinary = Join-Path $output 'stl-rep-parser.exe'
& go -C (Join-Path $PSScriptRoot 'parser') build -mod=readonly -trimpath -o $parserBinary .
if ($LASTEXITCODE -ne 0) { throw "Go build exited with code $LASTEXITCODE." }
Get-Item -LiteralPath $parserBinary
