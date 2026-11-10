@echo off
REM One-shot build: DLL + self-installing DXV-Codec-Setup.exe
REM Prereqs: Visual Studio 2022 (C++ workload), CMake, vcpkg (VCPKG_ROOT set), Inno Setup 6.
setlocal
if "%VCPKG_ROOT%"=="" (echo Set VCPKG_ROOT first & exit /b 1)
"%VCPKG_ROOT%\vcpkg" install ffmpeg[core,avcodec,swscale]:x64-windows-static || exit /b 1
cmake -B build -A x64 -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static || exit /b 1
cmake --build build --config Release || exit /b 1
"%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" installer\dxv_codec.iss || exit /b 1
echo.
echo Done: installer\Output\DXV-Codec-Setup.exe
