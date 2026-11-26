@echo off
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo [!] Visual Studio C++ Build Tools not found.
  exit /b 1
)
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

if not exist src\cogitator.ico powershell -NoProfile -ExecutionPolicy Bypass -File gen_icon.ps1 || exit /b 1
for %%d in (build bin dist) do if not exist %%d mkdir %%d

set CFLAGS=/nologo /O1 /Os /MT /Gy /GL /utf-8 /W3 /EHsc /DUNICODE /D_UNICODE
set LFLAGS=/LTCG /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /MANIFEST:EMBED /MANIFESTUAC:"level='asInvoker' uiAccess='false'" /MANIFESTDEPENDENCY:"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'"

rem ---- 1. widget
pushd src
rc /nologo /fo ..\build\cogitator.res cogitator.rc || (popd & exit /b 1)
popd
cl %CFLAGS% /Fobuild\ src\cogitator.cpp build\cogitator.res /Febin\Cogitator.exe /link %LFLAGS% || exit /b 1

rem ---- 2. installer (embeds bin\Cogitator.exe)
pushd src
rc /nologo /fo ..\build\setup.res setup.rc || (popd & exit /b 1)
popd
cl %CFLAGS% /Fobuild\ src\setup.cpp build\setup.res /Fedist\Cogitator_Setup_2.1.0.exe /link %LFLAGS% || exit /b 1

echo.
echo [OK] bin\Cogitator.exe
echo [OK] dist\Cogitator_Setup_2.1.0.exe
