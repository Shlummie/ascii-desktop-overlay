@echo off
setlocal
pushd "%~dp0"

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" goto missing_vcvars

call "%VCVARS%"
if errorlevel 1 exit /b %errorlevel%

cl /nologo /std:c++17 /EHsc /W4 /permissive- /O2 asciivision_native.cpp /Fe:ASCIIVisionNative.exe /link user32.lib gdi32.lib shell32.lib d3d11.lib dxgi.lib winmm.lib
set "BUILD_EXIT=%errorlevel%"

if "%BUILD_EXIT%"=="0" (
    echo Built ASCIIVisionNative.exe
)

popd
exit /b %BUILD_EXIT%

:missing_vcvars
echo Visual Studio Build Tools were not found at:
echo %VCVARS%
popd
exit /b 1
