@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
echo === ENV READY, STARTING BUILD ===
cmake --build "E:\xmathayus source\RE-UE4SS-vein-3.0.1-940\out\build\x64-Release" --target UE4SS
echo === BUILD EXIT CODE: %ERRORLEVEL% ===
