@echo off
rem Builds the Stream Bridge VST3 in Release mode, using the CMake and Ninja that come with Visual Studio.

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto noVisualStudio

set "VS="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS goto noVisualStudio

cd /d "%~dp0"

rem The small ffmpeg for the YouTube downloader. It is built once, with MSYS2 (https://www.msys2.org).
if exist "build\ffmpeg\ffmpeg.exe" goto ffmpegDone
if not defined MSYS2_ROOT set "MSYS2_ROOT=C:\msys64"
if exist "%MSYS2_ROOT%\usr\bin\bash.exe" goto buildFfmpeg
echo MSYS2 was not found in %MSYS2_ROOT%, so ffmpeg is not built. The YouTube downloader will not work.
goto ffmpegDone

:buildFfmpeg
echo Building ffmpeg, first time only. This takes a few minutes...
set "MSYSTEM=UCRT64"
set "CHERE_INVOKING=1"
"%MSYS2_ROOT%\usr\bin\bash.exe" -lc "./ffmpeg/build.sh build/ffmpeg" || exit /b 1

:ffmpegDone
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

set "VSCMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake"
if exist "%VSCMAKE%\CMake\bin\cmake.exe" set "PATH=%VSCMAKE%\CMake\bin;%VSCMAKE%\Ninja;%PATH%"

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build || exit /b 1

echo.
echo Plugin: %~dp0build\StreamBridge_artefacts\Release\VST3\Stream Bridge.vst3
exit /b 0

:noVisualStudio
echo Visual Studio with C++ was not found.
echo Install "Build Tools for Visual Studio" with the "Desktop development with C++" workload.
exit /b 1
