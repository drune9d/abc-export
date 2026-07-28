@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "ROOT_DIR=%~dp0"
if "%ROOT_DIR:~-1%"=="\" set "ROOT_DIR=%ROOT_DIR:~0,-1%"

if not "%~1"=="" (
  set "HOUDINI_ROOT=%~1"
) else if not defined HOUDINI_ROOT (
  if defined HFS (
    set "HOUDINI_ROOT=%HFS%"
  ) else (
    for /f "usebackq delims=" %%I in (`powershell -NoProfile -ExecutionPolicy Bypass -Command "$base = 'C:\Program Files\Side Effects Software'; if (Test-Path $base) { Get-ChildItem $base -Directory | Where-Object { $_.Name -match '^Houdini [0-9]+(\.[0-9]+)*$' } | Sort-Object { [version]($_.Name -replace '^Houdini ', '') } -Descending | Select-Object -First 1 -ExpandProperty FullName }"`) do set "HOUDINI_ROOT=%%I"
  )
)

if not defined HOUDINI_ROOT (
  echo Houdini was not found.
  echo Pass the Houdini install folder, for example:
  echo   build.bat "C:\Program Files\Side Effects Software\Houdini 21.0.700"
  exit /b 1
)

set "CMAKE_EXE="
for /f "delims=" %%I in ('where cmake 2^>nul') do if not defined CMAKE_EXE set "CMAKE_EXE=%%I"
if not defined CMAKE_EXE (
  set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  if exist "!VSWHERE!" (
    for /f "usebackq delims=" %%I in (`powershell -NoProfile -ExecutionPolicy Bypass -Command "& '!VSWHERE!' -latest -products * -requires Microsoft.VisualStudio.Component.VC.CMake.Project -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'"`) do if not defined CMAKE_EXE set "CMAKE_EXE=%%I"
    for /f "usebackq delims=" %%I in (`powershell -NoProfile -ExecutionPolicy Bypass -Command "& '!VSWHERE!' -latest -products * -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'"`) do if not defined CMAKE_EXE set "CMAKE_EXE=%%I"
  )
)

if not defined CMAKE_EXE (
  echo CMake is required but was not found.
  echo Install CMake or the Visual Studio C++ CMake tools, then run this script again.
  exit /b 1
)

where cl >nul 2>nul
if errorlevel 1 (
  set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  if exist "!VSWHERE!" (
    set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;!PATH!"
    for /f "usebackq delims=" %%I in (`powershell -NoProfile -ExecutionPolicy Bypass -Command "& '!VSWHERE!' -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath"`) do set "VS_INSTALL=%%I"
  )
  if defined VS_INSTALL if exist "!VS_INSTALL!\VC\Auxiliary\Build\vcvars64.bat" (
    call "!VS_INSTALL!\VC\Auxiliary\Build\vcvars64.bat" >nul
  )
)

where cl >nul 2>nul
if errorlevel 1 (
  echo Visual Studio C++ build tools were not found.
  echo Install the Desktop development with C++ workload, then run this script again.
  exit /b 1
)

if not exist "%ROOT_DIR%\build" mkdir "%ROOT_DIR%\build"

"%CMAKE_EXE%" -S "%ROOT_DIR%" -B "%ROOT_DIR%\build" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DHOUDINI_ROOT="%HOUDINI_ROOT%"
if errorlevel 1 exit /b 1

"%CMAKE_EXE%" --build "%ROOT_DIR%\build" --config Release
if errorlevel 1 exit /b 1

if not exist "%ROOT_DIR%\bin" mkdir "%ROOT_DIR%\bin"
if exist "%ROOT_DIR%\build\mesh2abc.exe" copy /Y "%ROOT_DIR%\build\mesh2abc.exe" "%ROOT_DIR%\bin\mesh2abc.exe" >nul

echo.
echo Build complete: %ROOT_DIR%\bin\mesh2abc.exe
