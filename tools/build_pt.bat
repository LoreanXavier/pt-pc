@echo off
setlocal
set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=release"
set "BUILD_NAME=%~2"
if "%BUILD_NAME%"=="" set "BUILD_NAME=%PRESET%"
set "TARGET_ARGS="
if not "%~3"=="" set "TARGET_ARGS=--target %~3"
set "VSDIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
set "PATH=%SystemRoot%\system32;%SystemRoot%;%SystemRoot%\System32\Wbem;%SystemRoot%\System32\WindowsPowerShell\v1.0;C:\Program Files\Git\cmd;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if "%VULKAN_SDK%"=="" set "VULKAN_SDK=C:\VulkanSDK\1.4.357.0"
set "PATH=C:\Program Files\LLVM\bin;%VULKAN_SDK%\Bin;%PATH%"
cd /d "%~dp0.." || exit /b 1
set "BUILD_DIR=build\%BUILD_NAME%"
set "SHARED="
set "DEPS=%PT_DEPS%"
if "%DEPS%"=="" if /i not "%BUILD_NAME%"=="%PRESET%" set "DEPS=%CD%\build\release\_deps"
if not "%DEPS%"=="" if exist "%DEPS%\sdl3-src" (
  set "SHARED=-DFETCHCONTENT_SOURCE_DIR_SDL3=%DEPS%\sdl3-src -DFETCHCONTENT_SOURCE_DIR_ZLIB=%DEPS%\zlib-src -DFETCHCONTENT_SOURCE_DIR_VOLK=%DEPS%\volk-src -DFETCHCONTENT_SOURCE_DIR_VMA=%DEPS%\vma-src -DFETCHCONTENT_SOURCE_DIR_GLM=%DEPS%\glm-src -DFETCHCONTENT_SOURCE_DIR_IMGUI=%DEPS%\imgui-src -DFETCHCONTENT_SOURCE_DIR_STB=%DEPS%\stb-src -DFETCHCONTENT_SOURCE_DIR_LUA51=%DEPS%\lua51-src -DFETCHCONTENT_SOURCE_DIR_OGG=%DEPS%\ogg-src -DFETCHCONTENT_SOURCE_DIR_VORBIS=%DEPS%\vorbis-src -DPT_UPSCALER_SDK_DIR=%DEPS%\upscaler-sdks"
)
if not "%DEPS%"=="" if exist "%DEPS%\whisper-src" set "SHARED=%SHARED% -DFETCHCONTENT_SOURCE_DIR_WHISPER=%DEPS%\whisper-src"
if not exist "%BUILD_DIR%\CMakeCache.txt" cmake --preset %PRESET% -B "%BUILD_DIR%" %SHARED% || exit /b 2
cmake --build "%BUILD_DIR%" --parallel %NUMBER_OF_PROCESSORS% %TARGET_ARGS% || exit /b 3
echo BUILD_OK %CD%\%BUILD_DIR%\pt.exe
