@echo off
set PS5_PAYLOAD_SDK=C:\Users\Blurf\ps5dev\toolchains\ps5-payload-sdk
set PATH=%PS5_PAYLOAD_SDK%\bin;%PATH%
make CC=ps5-clang.cmd CXX=ps5-c++.cmd
