@echo off
setlocal EnableExtensions DisableDelayedExpansion
set "recorder=%USERPROFILE%\NeuralAliasAbundance\local\Lighting-Test-Resistance-Discovery-Poise"
if exist "%recorder%\INSTALL.cmd" goto run
echo The Resistance recorder package could not be found at:
echo "%recorder%"
pause
exit /b 1
:run
"%recorder%\INSTALL.cmd"
