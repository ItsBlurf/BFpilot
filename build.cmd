@echo off
setlocal
if not defined PS5_PAYLOAD_SDK (
  echo Set PS5_PAYLOAD_SDK to your PS5 payload SDK directory first. 1>&2
  exit /b 2
)
if not exist "%PS5_PAYLOAD_SDK%\toolchain\prospero.mk" (
  echo PS5_PAYLOAD_SDK does not contain toolchain\prospero.mk. 1>&2
  exit /b 2
)
set "PATH=%PS5_PAYLOAD_SDK%\win;%PS5_PAYLOAD_SDK%\bin;%PATH%"
pushd "%~dp0"
if errorlevel 1 exit /b 2
make CC=prospero-clang.cmd CXX=prospero-clang++.cmd PYTHON=python %*
set "bfpilot_build_result=%ERRORLEVEL%"
popd
exit /b %bfpilot_build_result%
