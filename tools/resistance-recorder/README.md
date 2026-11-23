The standalone launchers use the staged Resistance recorder in `%USERPROFILE%\NeuralAliasAbundance\local\Lighting-Test-Resistance-Discovery-Poise`. A download can be run directly.

Configure the normal Windows Release build with `-C cmake/lighting-recorder.cmake` and the project's SDKs. Build NeuralAliasDXGI. From the checkout, run:

    python tools/resistance-recorder/stage.py <previous-complete-package> <new-NeuralAliasDXGI.dll> <full-source-commit> <new-package-directory>

The packager checks the previous payload hashes and boots the replacement DLL in an isolated process to confirm scene discovery starts. It verifies the embedded Poise Light, Regular and Medium fonts and their license. Staging leaves the game installation to Cort. Rehearse the existing receipt's upgrade and rollback in a disposable game directory before distributing the launcher.
