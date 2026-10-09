@echo off
rem wayland-scanner for the Linux cross build on Windows: SDL3's Wayland backend needs it to generate the protocol code and
rem there is no Windows build, so this runs the one in WSL (in the distribution: apt install libwayland-bin) and turns
rem Windows paths (C:\x or C:/x) into /mnt/c/x. PT_WSL_DISTRO picks the distribution (default Ubuntu-24.04).
setlocal EnableDelayedExpansion
set "DISTRO=%PT_WSL_DISTRO%"
if "%DISTRO%"=="" set "DISTRO=Ubuntu-24.04"
set "ARGS="
:next
if "%~1"=="" goto run
set "a=%~1"
set "a=!a:\=/!"
if "!a:~1,2!"==":/" (
  set "drive=!a:~0,1!"
  for %%L in (a b c d e f g h) do set "drive=!drive:%%L=%%L!"
  set "a=/mnt/!drive!!a:~2!"
)
set ARGS=!ARGS! "!a!"
shift
goto next
:run
wsl.exe -d %DISTRO% -- wayland-scanner !ARGS!
exit /b %ERRORLEVEL%
