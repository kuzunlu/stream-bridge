@echo off
rem Builds the Stream Bridge VST3 in Release mode, using the CMake and Ninja that come with Visual Studio.

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto noVisualStudio

set "VS="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS goto noVisualStudio

call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

set "VSCMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake"
if exist "%VSCMAKE%\CMake\bin\cmake.exe" set "PATH=%VSCMAKE%\CMake\bin;%VSCMAKE%\Ninja;%PATH%"

cd /d "%~dp0"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build || exit /b 1

echo.
echo Plugin: %~dp0build\StreamBridge_artefacts\Release\VST3\Stream Bridge.vst3
exit /b 0

:noVisualStudio
echo Visual Studio with C++ was not found.
echo Install "Build Tools for Visual Studio" with the "Desktop development with C++" workload.
exit /b 1
