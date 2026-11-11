@echo off
rem Makes the bots learn a map's routes again from scratch. The old routes are kept next to the new ones as a .bak file.
rem reset-bot-routes.bat            asks which map
rem reset-bot-routes.bat MP_03      resets that map
setlocal EnableDelayedExpansion
cd /d "%~dp0"
title MOHW Bots - reset bot routes

if not exist "mohw-bots-nav\" (
    echo No mohw-bots-nav folder here. Put this file in your game folder, next to lanmohw.exe.
    goto :end
)
"%SystemRoot%\System32\tasklist.exe" /fi "imagename eq lanmohw.exe" 2>nul | "%SystemRoot%\System32\find.exe" /i "lanmohw.exe" >nul
if not errorlevel 1 (
    echo The game is running, and it would write the routes straight back. Close it first.
    echo While playing you can use ProCon's console instead: nav.reset
    goto :end
)

set count=0
for %%L in ("MP_03|Somalia Stronghold" "MP_05|Novi Grad Warzone" "MP_10|Sarajevo Stadium" "MP_12|Basilan Aftermath" "MP_13|Hara Dunes" "MP_16|Al Fara Cliffside" "MP_18|Shogore Valley" "MP_19|Tungawan Jungle" "MP_20|Darra Gun Market" "MP_21|Chitral Compound") do (
    for /f "tokens=1,2 delims=|" %%A in ("%%~L") do (
        if exist "mohw-bots-nav\%%A_*.nav" (
            set /a count+=1
            set "code!count!=%%A"
            set "name!count!=%%B"
        )
    )
)
if %count%==0 (
    echo No map has learned routes yet, so there's nothing to reset.
    goto :end
)

set "pick="
if not "%~1"=="" (
    for /l %%N in (1,1,%count%) do if /i "!code%%N!"=="%~1" set "pick=%%N"
    if not defined pick (
        echo %~1 has no learned routes.
        goto :end
    )
    goto :reset
)

echo Which map should the bots learn again from scratch?
echo.
for /l %%N in (1,1,%count%) do echo   %%N. !name%%N!
echo.
set /p "pick=Number (or Enter to cancel): "
if not defined pick (
    echo Nothing changed.
    goto :end
)
set "valid="
for /l %%N in (1,1,%count%) do if "!pick!"=="%%N" set "valid=1"
if not defined valid (
    echo That isn't one of the numbers. Nothing changed.
    goto :end
)

:reset
set "code=!code%pick%!"
set "name=!name%pick%!"
set "stamp=1"
:nextstamp
if exist "mohw-bots-nav\%code%_*.nav.reset%stamp%.bak" (
    set /a stamp+=1
    goto :nextstamp
)
for %%F in ("mohw-bots-nav\%code%_*.nav") do ren "%%F" "%%~nxF.reset%stamp%.bak"
echo.
echo Routes reset for %name%. The bots start learning it again next time you play it.
echo The old routes are in mohw-bots-nav\ as %code%_*.nav.reset%stamp%.bak. To get them back, delete
echo the new .nav file for that map and take .reset%stamp%.bak off the old file's name.

:end
echo.
if "%~1"=="" pause
endlocal
