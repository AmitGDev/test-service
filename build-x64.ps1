param(
    [switch]$Clean
)

if ($Clean -and (Test-Path build)) {
    Remove-Item -Recurse -Force build
}

$vcvars = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat"

cmd /c "`"$vcvars`" x64 && set" | ForEach-Object {
    if ($_ -match '^(.*?)=(.*)$') {
        [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2])
    }
}

cmake --preset x64-debug
cmake --build --preset x64-debug