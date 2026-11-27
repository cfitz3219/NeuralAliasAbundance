@echo off
setlocal
set "launcher=%~dp0resistance-nr-model.ps1"
if not exist "%launcher%" set "launcher=%USERPROFILE%\NeuralAliasAbundance\local\Resistance-Neural-Rendering\resistance-nr-model.ps1"
if not exist "%launcher%" (
  echo Missing the Resistance NR model package at "%launcher%".
  pause
  exit /b 1
)
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%launcher%" -Action Install
set "result=%ERRORLEVEL%"
echo.
pause
exit /b %result%
