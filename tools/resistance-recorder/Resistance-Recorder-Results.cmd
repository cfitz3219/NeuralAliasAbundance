@echo off
setlocal EnableExtensions DisableDelayedExpansion
set "recorder=%USERPROFILE%\NeuralAliasAbundance\local\Lighting-Test-Resistance-d9106460"
if exist "%recorder%\RESULTS.cmd" goto run
echo The Resistance recorder package could not be found at:
echo "%recorder%"
pause
exit /b 1
:run
"%recorder%\RESULTS.cmd"
