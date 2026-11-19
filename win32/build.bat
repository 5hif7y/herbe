@echo off
rem Build herbe.exe with MSVC. Run from a "x64 Native Tools" prompt (or let vcvars be found).
setlocal
cd /d "%~dp0"
where cl >nul 2>nul || call "%ProgramFiles%\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if not exist config.h copy config.def.h config.h >nul
cl /nologo /W4 /O2 /utf-8 /std:c11 /D_CRT_SECURE_NO_WARNINGS herbe.c /Fe:herbe.exe /link user32.lib gdi32.lib shcore.lib
