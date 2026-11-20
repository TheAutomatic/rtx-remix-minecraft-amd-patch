@echo off
setlocal enabledelayedexpansion

echo ===================================================
echo  RTX Remix for Minecraft - AMD Patch Bridge Builder
echo ===================================================

set "SCRIPT_DIR=%~dp0"
cd /d "%SCRIPT_DIR%"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"

set "VS_DIR="
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        set "VS_DIR=%%i"
    )
)

if not defined VS_DIR (
    if exist "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools" (
        set "VS_DIR=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Community" (
        set "VS_DIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
    ) else (
        echo [ERROR] Visual Studio C++ toolset not found.
        exit /b 1
    )
)

echo Found Visual Studio at: %VS_DIR%

:: First attempt default vcvarsall x64
if exist "%VS_DIR%\VC\Auxiliary\Build\vcvarsall.bat" (
    call "%VS_DIR%\VC\Auxiliary\Build\vcvarsall.bat" x64
)

:: Verify if cl is available; if not, probe toolset version
where cl >nul 2>nul
if errorlevel 1 (
    echo Attempting toolset vcvars_ver=14.44.35207...
    if exist "%VS_DIR%\VC\Auxiliary\Build\vcvarsall.bat" (
        call "%VS_DIR%\VC\Auxiliary\Build\vcvarsall.bat" x64 -vcvars_ver=14.44.35207
    )
)

where cl >nul 2>nul
if errorlevel 1 (
    echo [ERROR] cl.exe compiler not found in PATH after vcvarsall.
    exit /b 1
)

if not exist "%SCRIPT_DIR%bin" mkdir "%SCRIPT_DIR%bin"
if not exist "%SCRIPT_DIR%build" mkdir "%SCRIPT_DIR%build"

echo Compiling remixbridge.dll...

cl /nologo /O2 /Zi /LD /EHsc /std:c++17 ^
   /I"%SCRIPT_DIR%include" /I"%SCRIPT_DIR%include\win32" ^
   "%SCRIPT_DIR%src\remixbridge.cpp" ^
   /Fe"%SCRIPT_DIR%bin\remixbridge.dll" ^
   /Fo"%SCRIPT_DIR%build\remixbridge.obj" ^
   /Fd"%SCRIPT_DIR%build\remixbridge.pdb" ^
   /link /nologo /DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF dbghelp.lib opengl32.lib user32.lib gdi32.lib

if exist "%SCRIPT_DIR%bin\remixbridge.ilk" del /q "%SCRIPT_DIR%bin\remixbridge.ilk"

if %ERRORLEVEL% equ 0 (
    echo.
    echo ===================================================
    echo  Build Succeeded!
    echo  Output: bin\remixbridge.dll
    echo ===================================================
) else (
    echo.
    echo [ERROR] Build Failed with exit code %ERRORLEVEL%
    exit /b %ERRORLEVEL%
)

endlocal
