@echo off
setlocal
pushd "%~dp0"

if not exist "ASCIIVisionNative.exe" (
    call "Build ASCIIVision Native.bat"
    if errorlevel 1 (
        popd
        pause
        exit /b 1
    )
)

start "" "%~dp0ASCIIVisionNative.exe"
popd
exit /b 0
