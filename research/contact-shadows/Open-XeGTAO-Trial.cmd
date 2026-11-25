@echo off
setlocal
set "trial=%~dp0XeGTAO-trial.html"
if not exist "%trial%" set "trial=%USERPROFILE%\NeuralAliasAbundance\local\XeGTAO-Referee-Trial\XeGTAO-trial.html"
if not exist "%trial%" (
  echo The local XeGTAO visual trial was not found.
  pause
  exit /b 1
)
start "" "%trial%"
