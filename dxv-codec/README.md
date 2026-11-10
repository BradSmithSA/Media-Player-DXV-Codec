# DXV decoder for Windows 11 (Media Foundation)

A Media Foundation Transform (MFT) that decodes Resolume DXV3 video (`DXD3`/`DXDI` in MOV)
by wrapping FFmpeg's DXV decoder. Output is NV12 or RGB32.

## Build
1. `vcpkg install ffmpeg[avcodec,swscale]:x64-windows-static`  (default LGPL build)
2. `cmake -B build -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static`
3. `cmake --build build --config Release`
4. Compile `installer/dxv_codec.iss` with Inno Setup 6 -> `DXV-Codec-Setup.exe`

Manual test without the installer (admin shell): `regsvr32 build\Release\dxv_mft.dll`

## Make a test file
`ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30 -t 5 -c:v dxv test.mov`

## Licensing
FFmpeg is LGPL 2.1+. Statically linking it means you must let users relink
(ship object files/build instructions) and include the license text. Dynamic
linking with the FFmpeg DLLs next to dxv_mft.dll is the simpler route.
DXV is Resolume's proprietary format; FFmpeg's decoder is reverse-engineered.

## Known risks (untested)
* Written without a Windows toolchain: expect to fix compile errors.
* The Windows MP4/MOV source must expose the DXD3 track to the pipeline. If Media Player
  still says "codec not supported", the source is rejecting the unknown sample entry and
  you'd need a custom IMFByteStreamHandler/MediaSource for .mov (bigger job).
* 64-bit only. Use `mfplay`/Media Player and `MFTrace` to debug.

## Getting the self-installing .exe
* Local: run `build.bat` -> `installer\Output\DXV-Codec-Setup.exe` (double-click to install; it asks for admin).
* Cloud: push this folder to a GitHub repo; the `build-installer` workflow produces the .exe as a downloadable artifact (Actions tab -> run -> Artifacts).
* Silent install: `DXV-Codec-Setup.exe /VERYSILENT /SUPPRESSMSGBOXES`
