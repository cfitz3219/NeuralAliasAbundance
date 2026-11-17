@echo off
setlocal
set "APP=%~dp0tools\growth_checker\local_app.py"
if defined GAMECHECK_PYTHON if exist "%GAMECHECK_PYTHON%" (
    start "" "%GAMECHECK_PYTHON%" -B "%APP%"
    exit /b
)
for /d %%D in ("%LOCALAPPDATA%\hermes\tools\python-*") do (
    if exist "%%~fD\pythonw.exe" (
        start "" "%%~fD\pythonw.exe" -B "%APP%"
        exit /b
    )
)
where pyw.exe >nul 2>&1
if not errorlevel 1 (
    start "" pyw.exe -3 -B "%APP%"
    exit /b
)
where pythonw.exe >nul 2>&1
if not errorlevel 1 (
    start "" pythonw.exe -B "%APP%"
    exit /b
)
echo GameCheck needs Python 3 with Tk support to open the results folder picker.
pause
exit /b 1
