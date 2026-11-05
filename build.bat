@echo off
setlocal
rem Compila Jolt y la herramienta de medicion con MSVC (sin CMake).
rem   build.bat          -> optimizado (/O2)
rem   build.bat rapido   -> sin optimizar (/Od), para iterar
rem   build.bat limpiar  -> borra build\ (incluidos los headers precompilados y el entorno guardado)
rem   build.bat correr   -> cierra Jolt si esta abierto, compila y lo vuelve a abrir (admite: correr rapido)
cd /d "%~dp0"
if "%1"=="limpiar" (rmdir /s /q build 2>nul & echo Limpio. & exit /b 0)
set CORRER=0
if "%1"=="correr" (set CORRER=1& shift)
if not exist build mkdir build
rem Jolt queda abierto escondido: hay que pedirle que salga para poder reemplazar el ejecutable
if exist build\jolt.exe build\jolt.exe --salir
type nul > build\jolt.dev

rem --- entorno del compilador: vcvars64.bat tarda ~1,3 s; se guarda la primera vez y se reutiliza
if exist build\env.bat (call build\env.bat & goto env_ok)
set VSDIR=
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%%VSWHERE%%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo No se encontro Visual Studio con C++ & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
(echo set "PATH=%PATH%" & echo set "INCLUDE=%INCLUDE%" & echo set "LIB=%LIB%" & echo set "LIBPATH=%LIBPATH%") > build\env.bat
:env_ok

set MODE=O2
if "%1"=="rapido" set MODE=Od
set FLAGS=/nologo /std:c++20 /%MODE% /W4 /EHsc /utf-8 /permissive- /MT /Zi /DUNICODE /D_UNICODE /DNOMINMAX /Fdbuild\
set LINKF=/link /DEBUG /OPT:REF /OPT:ICF

rem --- headers del sistema (~580 mil lineas): se procesan una sola vez por modo
set PCHFILE=build\pch-%MODE%.pch
if not exist %PCHFILE% cl %FLAGS% /c /Yc"pch.h" /Fp%PCHFILE% /Fobuild\pch-%MODE%.obj /Isrc src\pch.cpp || exit /b 1
set PCH=/Yu"pch.h" /FI"pch.h" /Fp%PCHFILE% /Isrc

rc /nologo /fo build\jolt.res src\jolt.rc || exit /b 1

cl %FLAGS% %PCH% /Fobuild\ /Febuild\jolt.exe src\app.cpp src\dirlist.cpp build\pch-%MODE%.obj build\jolt.res %LINKF% /SUBSYSTEM:WINDOWS ^
  d2d1.lib dwrite.lib user32.lib gdi32.lib shell32.lib shlwapi.lib ole32.lib comctl32.lib dwmapi.lib windowscodecs.lib uuid.lib advapi32.lib oleaut32.lib || exit /b 1

cl %FLAGS% %PCH% /Fobuild\ /Febuild\bench.exe src\bench.cpp build\dirlist.obj build\pch-%MODE%.obj %LINKF% /SUBSYSTEM:CONSOLE ^
  shell32.lib shlwapi.lib ole32.lib uuid.lib || exit /b 1

echo.
echo Listo (%MODE%): build\jolt.exe y build\bench.exe
if "%CORRER%"=="1" start "" "%~dp0build\jolt.exe"