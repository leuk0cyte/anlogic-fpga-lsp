@echo off
REM Audit helper: run one command with the MSVC x64 environment and LLVM on PATH.
REM Needed because cl.exe is not on PATH in this environment and clang (GNU driver)
REM requires the MSVC/STL include directories that vcvars64.bat sets up.
REM
REM   cmd /c tools\audit\with_vcvars.cmd <command> [args...]
setlocal enableextensions
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo [with_vcvars] vcvars64.bat failed 1>&2
  exit /b 900
)
set "PATH=%LOCALAPPDATA%\scoop\apps\llvm\current\bin;%PATH%"
cd /d "%~dp0..\.."
%*
exit /b %errorlevel%