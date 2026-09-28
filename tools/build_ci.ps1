param(
    [string]$Configuration = "Release"
)

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$out = Join-Path $root 'bin'
$obj = Join-Path $out 'obj-ci'
New-Item -ItemType Directory -Force $out | Out-Null
New-Item -ItemType Directory -Force $obj | Out-Null
# Only clear compiler outputs in this script's dedicated object directory. This
# prevents renamed source files from leaving stale .obj files in the link step.
Get-ChildItem -LiteralPath $obj -Filter '*.obj' -File -ErrorAction SilentlyContinue |
    Remove-Item -Force

$vs = & "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -property installationPath
if (!$vs) { throw 'Visual Studio installation not found.' }

$cflags = '/nologo /D_CRT_SECURE_NO_WARNINGS /std:c11 /I"' + $root + '" /c '
$cppflags = '/nologo /EHsc /std:c++17 /I"' + $root + '" /c "' + (Join-Path $PSScriptRoot 'ci_cli.cpp') + '"'
if ($Configuration -ieq 'Debug') {
    $cflags = '/Od /Zi ' + $cflags
    $cppflags = '/Od /Zi ' + $cppflags
} else {
    $cflags = '/O2 /DNDEBUG ' + $cflags
    $cppflags = '/O2 /DNDEBUG ' + $cppflags
}

$sources = @(
    'array.c', 'AST.c', 'CodeGen.c', 'Debugger.c', 'Emitter.c', 'Ownership.c',
    'lexer.c', 'opcode.c', 'Optimizer.c', 'Parser.c', 'SymbolTable.c',
    'types.c', 'VM.c'
) | ForEach-Object { '"' + (Join-Path $root $_) + '"' }

$commands = @(
    ('@call "' + $vs + '\VC\Auxiliary\Build\vcvars64.bat" >nul'),
    ('cl ' + $cflags + ($sources -join ' ')),
    'if errorlevel 1 exit /b 1',
    ('cl ' + $cppflags),
    'if errorlevel 1 exit /b 1',
    ('link /nologo *.obj /OUT:"' + (Join-Path $out 'ci.exe') + '"'),
    'if errorlevel 1 exit /b 1'
)

Set-Content -LiteralPath (Join-Path $obj 'build.cmd') -Value $commands
Push-Location $obj
try {
    & cmd.exe /c build.cmd
    if ($LASTEXITCODE) { throw 'ci.exe build failed' }
}
finally {
    Pop-Location
}

Write-Host "Built $out\ci.exe"
