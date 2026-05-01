@echo off
rem ===================================================================
rem ACECode Windows installer (double-click to run)
rem
rem 1) Adds the directory containing this .bat to the user PATH
rem    (HKCU\Environment) so `acecode` works in any new terminal.
rem 2) Extracts skills.zip (next to this .bat) to
rem    %USERPROFILE%\.acecode\skills.
rem 3) If config.json sits next to this .bat, copies it to
rem    %USERPROFILE%\.acecode\config.json. An existing config is
rem    moved to config.json.bak first.
rem
rem Pure cmd. No PowerShell, no external tools other than what ships
rem with Windows: reg.exe + tar.exe (tar is built-in since Windows 10
rem build 17063, i.e. 1803+).
rem
rem No administrator rights required.
rem ===================================================================

setlocal EnableExtensions EnableDelayedExpansion

title ACECode Setup

set "INSTALL_DIR=%~dp0"
if "%INSTALL_DIR:~-1%"=="\" set "INSTALL_DIR=%INSTALL_DIR:~0,-1%"

set "SKILLS_ZIP=%INSTALL_DIR%\skills.zip"
set "ACECODE_HOME=%USERPROFILE%\.acecode"
set "SKILLS_TARGET=%ACECODE_HOME%\skills"

echo ===================================================
echo  ACECode Setup
echo ===================================================
echo  Install dir   : %INSTALL_DIR%
echo  Skills target : %SKILLS_TARGET%
echo  Skills zip    : %SKILLS_ZIP%
echo ---------------------------------------------------

if not exist "%INSTALL_DIR%\acecode.exe" (
    echo.
    echo Warning: acecode.exe was not found in this directory.
    echo Setup will continue, but verify you placed the .bat next to acecode.exe.
)

echo.
echo [1/3] Adding install directory to user PATH ...
call :update_path

echo.
echo [2/3] Installing default skills ...
call :install_skills

echo.
echo [3/3] Installing config.json ...
call :install_config

echo.
echo ===================================================
echo  Setup finished. Press any key to close.
echo ===================================================
pause >nul
endlocal
exit /b 0

rem -------------------------------------------------------------------
rem Append %INSTALL_DIR% to HKCU\Environment\Path if not already there.
rem Preserves the original registry value type (REG_EXPAND_SZ or REG_SZ).
rem Uses reg.exe directly to avoid setx.exe's 1024-character truncation.
rem -------------------------------------------------------------------
:update_path
set "USER_PATH="
set "USER_PATH_TYPE=REG_EXPAND_SZ"
for /f "tokens=1,2,*" %%A in ('reg query "HKCU\Environment" /v Path 2^>nul') do (
    if /I "%%A"=="Path" (
        set "USER_PATH_TYPE=%%B"
        set "USER_PATH=%%C"
    )
)

set "ALREADY=0"
if defined USER_PATH (
    for %%P in ("!USER_PATH:;=" "!") do (
        set "ITEM=%%~P"
        if defined ITEM (
            if "!ITEM:~-1!"=="\" set "ITEM=!ITEM:~0,-1!"
            if /I "!ITEM!"=="%INSTALL_DIR%" set "ALREADY=1"
        )
    )
)

if "!ALREADY!"=="1" (
    echo   - already on user PATH: %INSTALL_DIR%
    exit /b 0
)

if defined USER_PATH (
    set "NEW_PATH=!USER_PATH!;%INSTALL_DIR%"
) else (
    set "NEW_PATH=%INSTALL_DIR%"
)

reg add "HKCU\Environment" /v Path /t !USER_PATH_TYPE! /d "!NEW_PATH!" /f >nul
if errorlevel 1 (
    echo   - PATH update failed.
    exit /b 1
)
echo   - added: %INSTALL_DIR%
echo   - open a NEW terminal for the change to take effect.
exit /b 0

rem -------------------------------------------------------------------
rem Extract skills.zip into %SKILLS_TARGET%. Uses tar.exe (built-in on
rem Windows 10 1803+). Existing files of the same path are overwritten.
rem -------------------------------------------------------------------
:install_skills
if not exist "%SKILLS_ZIP%" (
    echo   - skills.zip not found at "%SKILLS_ZIP%", skipping.
    exit /b 0
)

where tar >nul 2>&1
if errorlevel 1 (
    echo   - tar.exe not found ^(needs Windows 10 1803+^), skipping skill install.
    exit /b 1
)

if not exist "%ACECODE_HOME%" mkdir "%ACECODE_HOME%" 2>nul
if not exist "%SKILLS_TARGET%" mkdir "%SKILLS_TARGET%" 2>nul

tar -xf "%SKILLS_ZIP%" -C "%SKILLS_TARGET%"
if errorlevel 1 (
    echo   - extract failed.
    exit /b 1
)
echo   - extracted to %SKILLS_TARGET%
exit /b 0

rem -------------------------------------------------------------------
rem Copy config.json next to the .bat into %ACECODE_HOME%\config.json.
rem If a config.json already exists at the destination it is moved to
rem config.json.bak (overwriting any prior .bak) before the copy.
rem -------------------------------------------------------------------
:install_config
set "CONFIG_SRC=%INSTALL_DIR%\config.json"
set "CONFIG_DST=%ACECODE_HOME%\config.json"
set "CONFIG_BAK=%CONFIG_DST%.bak"

if not exist "%CONFIG_SRC%" (
    echo   - config.json not found next to installer, skipping.
    exit /b 0
)

if not exist "%ACECODE_HOME%" mkdir "%ACECODE_HOME%" 2>nul

if exist "%CONFIG_DST%" (
    move /Y "%CONFIG_DST%" "%CONFIG_BAK%" >nul
    if errorlevel 1 (
        echo   - failed to back up existing config.json, aborting.
        exit /b 1
    )
    echo   - existing config backed up to %CONFIG_BAK%
)

copy /Y "%CONFIG_SRC%" "%CONFIG_DST%" >nul
if errorlevel 1 (
    echo   - copy failed.
    exit /b 1
)
echo   - copied to %CONFIG_DST%
exit /b 0
