@echo off
setlocal
rem Builds Jolt and the benchmark tool with MSVC (no CMake).
rem   build.bat          -> optimized (/O2)
rem   build.bat fast     -> unoptimized (/Od), for iterating
rem   build.bat clean    -> deletes build\ (including the precompiled headers and the saved environment)
rem   build.bat run      -> closes Jolt if it is open, builds it and opens it again (also accepts: run fast)
cd /d "%~dp0"
if "%1"=="clean" (rmdir /s /q build 2>nul & echo Clean. & exit /b 0)
set RUN=0
if "%1"=="run" (set RUN=1& shift)
if not exist build mkdir build
rem Jolt stays open but hidden: it has to be asked to quit so the executable can be replaced
if exist build\jolt.exe build\jolt.exe --quit
type nul > build\jolt.dev

rem --- compiler environment: vcvars64.bat takes ~1.3 s; it is saved the first time and reused
if exist build\env.bat (call build\env.bat & goto env_ok)
set VSDIR=
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%%VSWHERE%%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo Visual Studio with C++ not found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
(echo set "PATH=%PATH%" & echo set "INCLUDE=%INCLUDE%" & echo set "LIB=%LIB%" & echo set "LIBPATH=%LIBPATH%") > build\env.bat
:env_ok

set MODE=O2
if "%1"=="fast" set MODE=Od
set FLAGS=/nologo /std:c++20 /%MODE% /W4 /EHsc /utf-8 /permissive- /MT /Zi /bigobj /DUNICODE /D_UNICODE /DNOMINMAX /Fdbuild\
set LINKF=/link /DEBUG /OPT:REF /OPT:ICF /PDBALTPATH:%%_PDB%%

rem --- system headers (~580k lines): processed only once per mode
set PCHFILE=build\pch-%MODE%.pch
if not exist %PCHFILE% cl %FLAGS% /c /Yc"pch.h" /Fp%PCHFILE% /Fobuild\pch-%MODE%.obj /Isrc src\pch.cpp || exit /b 1
set PCH=/Yu"pch.h" /FI"pch.h" /Fp%PCHFILE% /Isrc

rc /nologo /fo build\jolt.res src\jolt.rc || exit /b 1

cl %FLAGS% %PCH% /Fobuild\ /Febuild\jolt.exe src\app.cpp src\dirlist.cpp build\pch-%MODE%.obj build\jolt.res %LINKF% /SUBSYSTEM:WINDOWS ^
  d2d1.lib dwrite.lib user32.lib gdi32.lib shell32.lib shlwapi.lib ole32.lib comctl32.lib dwmapi.lib windowscodecs.lib uuid.lib advapi32.lib oleaut32.lib || exit /b 1

cl %FLAGS% %PCH% /Fobuild\ /Febuild\bench.exe src\bench.cpp build\dirlist.obj build\pch-%MODE%.obj %LINKF% /SUBSYSTEM:CONSOLE ^
  shell32.lib shlwapi.lib ole32.lib uuid.lib || exit /b 1

echo.
echo Done (%MODE%): build\jolt.exe and build\bench.exe
if "%RUN%"=="1" start "" "%~dp0build\jolt.exe"