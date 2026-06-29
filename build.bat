@echo off
if /I not "%VSCMD_ARG_TGT_ARCH%"=="x64" (
  echo Not an x64 shell. Open "x64 Native Tools Command Prompt for VS 2026".
  exit /b 1
)
set VCPKG_ROOT=C:\vcpkg
cmake --preset full-vcpkg && cmake --build --preset full-vcpkg && ctest --preset full-vcpkg --output-on-failure