@echo off
setlocal EnableExtensions DisableDelayedExpansion
set "recorder=%USERPROFILE%\NeuralAliasAbundance\local\Lighting-Test-Resistance-Graphite"
if exist "%recorder%\UNINSTALL.cmd" goto run
echo The Resistance recorder package could not be found at:
echo "%recorder%"
pause
exit /b 1
:run
"%recorder%\UNINSTALL.cmd"
