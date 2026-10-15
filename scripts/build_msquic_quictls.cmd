@echo off
rem Build msquic STATIC lib (msquic_lib) with quictls TLS backend, for the Cleat Agent.
rem
rem Self-bootstrapping: downloads msquic/quictls/xdp sources + Strawberry Perl
rem portable into Agent/out/msquic-src if missing (out/ is disposable; one
rem command restores the whole toolchain).
rem - openssl built by msquic's CMake superbuild via perl Configure + nmake.
rem - local patch: OPENSSL_CONFIG_FLAGS gets no-asm (no NASM needed).
rem - on success, installs msquic.lib + public headers into the layout that
rem   Agent/CMakeLists.txt expects: Agent/Common/ThirdParty/msquic/{include,x64/Release}
rem   QUIC is OFF by default in this tree; after this script completes, configure
rem   the agent with -DCLEAT_WITH_QUIC=ON to link the built static lib.
rem NOTE: keep this file ASCII-only.
setlocal EnableDelayedExpansion
chcp 65001 >nul

rem ---- locate Visual Studio (vswhere; any edition), fall back to PATH ----
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
)
if defined VSPATH (
  call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
) else (
  call vcvars64.bat >nul 2>&1
)
if errorlevel 1 (
  echo [FAIL] MSVC x64 toolchain not found ^(install VS 2022 or Build Tools, or run from a Developer Prompt^)
  exit /b 1
)

set "ROOT=%~dp0.."
set "MSQUIC_SRC=%ROOT%\Agent\out\msquic-src"
set "DL=%MSQUIC_SRC%\downloads"
set "PERL_DIR=%MSQUIC_SRC%\tools\perl\perl\bin"
set "MSQUIC_VER=2.6.1"
set "PERL_ZIP=https://github.com/StrawberryPerl/Perl-Dist-Strawberry/releases/download/SP_54231_64bit/strawberry-perl-5.42.3.1-64bit-portable.zip"
set "MSQUIC_TGZ=https://github.com/microsoft/msquic/archive/refs/tags/v2.6.1.tar.gz"
set "QUICTLS_TGZ=https://github.com/quictls/openssl/archive/refs/heads/main.tar.gz"
set "XDP_TGZ=https://github.com/microsoft/xdp-for-windows/archive/refs/heads/release/1.4.tar.gz"

if not exist "%DL%" mkdir "%DL%"

rem ---- bootstrap: Strawberry Perl portable ----
if not exist "%PERL_DIR%\perl.exe" (
  echo [bootstrap] fetching Strawberry Perl portable...
  if not exist "%DL%\perl-portable.zip" curl -sL --retry 2 -o "%DL%\perl-portable.zip" "%PERL_ZIP%"
  powershell -NoProfile -Command "Expand-Archive -Force '%DL%\perl-portable.zip' '%MSQUIC_SRC%\tools\perl'"
)
set "PATH=%PERL_DIR%;%PATH%"
perl -v | findstr /C:"This is perl" >nul
if errorlevel 1 (
  echo [FAIL] perl not available in PATH
  exit /b 1
)

rem ---- bootstrap: msquic source ----
if not exist "%MSQUIC_SRC%\msquic-%MSQUIC_VER%\CMakeLists.txt" (
  echo [bootstrap] fetching msquic v%MSQUIC_VER% source...
  if not exist "%DL%\msquic.tar.gz" curl -sL --retry 2 -o "%DL%\msquic.tar.gz" "%MSQUIC_TGZ%"
  tar -xzf "%DL%\msquic.tar.gz" -C "%MSQUIC_SRC%"
)

rem ---- bootstrap: quictls source (submodules/quictls) ----
if not exist "%MSQUIC_SRC%\msquic-%MSQUIC_VER%\submodules\quictls\Configure" (
  echo [bootstrap] fetching quictls source...
  if not exist "%DL%\quictls.tar.gz" curl -sL --retry 2 -o "%DL%\quictls.tar.gz" "%QUICTLS_TGZ%"
  tar -xzf "%DL%\quictls.tar.gz" -C "%MSQUIC_SRC%\msquic-%MSQUIC_VER%\submodules\quictls" --strip-components=1
)

rem ---- bootstrap: xdp-for-windows headers (datapath_raw_xdp_win.c needs it) ----
if not exist "%MSQUIC_SRC%\msquic-%MSQUIC_VER%\submodules\xdp-for-windows\published\external" (
  echo [bootstrap] fetching xdp-for-windows headers...
  if not exist "%DL%\xdp.tar.gz" curl -sL --retry 2 -o "%DL%\xdp.tar.gz" "%XDP_TGZ%"
  tar -xzf "%DL%\xdp.tar.gz" -C "%DL%"
  xcopy /E /I /Y "%DL%\xdp-for-windows-release-1.4\published" "%MSQUIC_SRC%\msquic-%MSQUIC_VER%\submodules\xdp-for-windows\published" >nul
)

rem ---- local patch: no-asm (replayable; no NASM needed) ----
powershell -NoProfile -Command "$f='%MSQUIC_SRC%\msquic-%MSQUIC_VER%\submodules\CMakeLists.txt'; (Get-Content $f) -replace 'no-deprecated --libdir=lib','no-deprecated no-asm --libdir=lib' | Set-Content $f"

rem ---- configure + build ----
cd /d "%MSQUIC_SRC%"
if not exist msquic-build-quictls\CMakeCache.txt (
  cmake -S msquic-%MSQUIC_VER% -B msquic-build-quictls -G "Visual Studio 17 2022" -A x64 -DQUIC_BUILD_SHARED=OFF -DQUIC_BUILD_TEST=OFF -DQUIC_BUILD_TOOLS=OFF -DQUIC_ENABLE_LOGGING=OFF -DQUIC_TLS_LIB=quictls -DQUIC_SOURCE_LINK=OFF -DQUIC_EMBED_GIT_HASH=OFF
  if errorlevel 1 (
    echo [FAIL] configure
    exit /b 1
  )
)
cmake --build msquic-build-quictls --config Release --target msquic_lib
if errorlevel 1 (
  echo [FAIL] build
  exit /b 1
)

rem ---- install into the layout expected by Agent/CMakeLists.txt ----
set "DEST=%ROOT%\Agent\Common\ThirdParty\msquic"
if not exist "%DEST%\x64\Release" mkdir "%DEST%\x64\Release"
if not exist "%DEST%\include" mkdir "%DEST%\include"
copy /Y "%MSQUIC_SRC%\msquic-build-quictls\bin\Release\msquic.lib" "%DEST%\x64\Release\msquic.lib" >nul
xcopy /E /I /Y "%MSQUIC_SRC%\msquic-%MSQUIC_VER%\src\inc\*" "%DEST%\include" >nul
if errorlevel 1 (
  echo [FAIL] install (copy msquic.lib / headers)
  exit /b 1
)
echo [OK] msquic static lib (quictls) built and installed to Agent\Common\ThirdParty\msquic
