@echo off
rem Install the OFFICIAL libsodium MSVC prebuilt package (trimmed) into the layout
rem expected by Agent/CMakeLists.txt:
rem
rem   Agent/Common/ThirdParty/libsodium/
rem     include/  (sodium.h + sodium/*.h)
rem     x64|Win32/<Config>/v143/static/libsodium.lib   (Debug + Release)
rem
rem The full official package is ~18 MB packed (ARM64 / dynamic / ltcg / pdb
rem variants included); this script keeps ONLY what the build consumes.
rem No compiler needed. Linux users: use your distro package instead
rem (e.g. apt install libsodium-dev) - CMake uses pkg-config there.
rem vcpkg users can skip this script entirely (unofficial-sodium is auto-detected).
rem NOTE: keep this file ASCII-only.
setlocal
set "ROOT=%~dp0.."
set "DEST=%ROOT%\Agent\Common\ThirdParty\libsodium"
set "DL=%ROOT%\Agent\out\libsodium-dl"
set "VER=1.0.22"
set "URL=https://download.libsodium.org/libsodium/releases/libsodium-%VER%-msvc.zip"

rem ---- fetch + unpack (cached under Agent/out, disposable) ----
if not exist "%DL%" mkdir "%DL%"
if not exist "%DL%\unpack\libsodium\include\sodium.h" (
  if not exist "%DL%\libsodium-%VER%-msvc.zip" (
    echo [bootstrap] fetching libsodium %VER% MSVC prebuilt package...
    curl -sL --retry 2 -o "%DL%\libsodium-%VER%-msvc.zip" "%URL%"
    if errorlevel 1 (
      echo [FAIL] download failed: %URL%
      exit /b 1
    )
  )
  echo [extract] unpacking...
  powershell -NoProfile -Command "Expand-Archive -Force '%DL%\libsodium-%VER%-msvc.zip' '%DL%\unpack'"
  if errorlevel 1 (
    echo [FAIL] unzip failed
    exit /b 1
  )
)

rem ---- install keep-set: include + per-arch static libs (Debug/Release) ----
if not exist "%DEST%\include" mkdir "%DEST%\include"
xcopy /E /I /Y "%DL%\unpack\libsodium\include\*" "%DEST%\include" >nul
if exist "%DL%\unpack\libsodium\LICENSE.txt" copy /Y "%DL%\unpack\libsodium\LICENSE.txt" "%DEST%\LICENSE.txt" >nul

for %%A in (x64 Win32) do (
  for %%C in (Debug Release) do (
    if exist "%DL%\unpack\libsodium\%%A\%%C\v143\static\libsodium.lib" (
      if not exist "%DEST%\%%A\%%C\v143\static" mkdir "%DEST%\%%A\%%C\v143\static"
      copy /Y "%DL%\unpack\libsodium\%%A\%%C\v143\static\libsodium.lib" "%DEST%\%%A\%%C\v143\static\libsodium.lib" >nul
    )
  )
)

rem ---- trim: drop anything outside the keep-set (idempotent; heals fat installs) ----
if exist "%DEST%\ARM64" rmdir /S /Q "%DEST%\ARM64"
for %%A in (x64 Win32) do (
  for %%C in (Debug Release) do (
    if exist "%DEST%\%%A\%%C\v143\dynamic" rmdir /S /Q "%DEST%\%%A\%%C\v143\dynamic"
    if exist "%DEST%\%%A\%%C\v143\ltcg"    rmdir /S /Q "%DEST%\%%A\%%C\v143\ltcg"
    del /Q "%DEST%\%%A\%%C\v143\static\*.pdb" >nul 2>&1
  )
)

if not exist "%DEST%\x64\Release\v143\static\libsodium.lib" (
  echo [FAIL] install incomplete: x64\Release\v143\static\libsodium.lib missing
  exit /b 1
)
echo [OK] libsodium %VER% installed ^(trimmed: include + x64/Win32 static^)
