The standalone launchers use the staged Resistance recorder in `%USERPROFILE%\NeuralAliasAbundance\local\Lighting-Test-Resistance-Discovery-Poise`. A download can be run directly.

Configure the normal Windows Release build with `-C cmake/lighting-recorder.cmake` and the project's SDKs. Build NeuralAliasDXGI. From the checkout, run:

    python tools/resistance-recorder/stage.py <previous-complete-package> <new-NeuralAliasDXGI.dll> <full-source-commit> <new-package-directory>

The packager checks the previous payload hashes and boots the replacement DLL in an isolated process to confirm scene discovery starts. It verifies the embedded Poise Light, Regular and Medium fonts and their license. Staging leaves the game installation to Cort. Rehearse the existing receipt's upgrade and rollback in a disposable game directory before distributing the launcher.

## Resistance Neural Rendering model

Place `resistance-nr-model.ps1`, `Install-Resistance-NR.cmd`, the verified NVIDIA 310.9.16.0 `nvngx_dlssnr.dll` and its license together in `%USERPROFILE%\NeuralAliasAbundance\local\Resistance-Neural-Rendering`. The launcher also resolves that installed package if it is opened from Downloads. With the game closed, Install adds the model beside the Resistance executable. Run the PowerShell script with `-Action Status` to verify the digest or `-Action Remove` to uninstall a package-owned model. A matching pre-existing model stays in place on removal. A different model requires a separate migration. The installer never edits the existing DLAA proxy or configuration.

The offline probe uses the project's D3D12 test fixture and a developer-supplied NGX SDK. Build from an x64 Visual Studio developer shell using the same SDK as the host. Substitute the SDK and package paths in the following commands.

```bat
cl /nologo /std:c++17 /O2 /EHsc /MT /DNOMINMAX /DUNICODE /D_UNICODE /Itests\host /I"%NGX_SDK%\include" tools\resistance-recorder\offline_nr_probe.cpp /Fe:"%NR_PACKAGE%\offline-nr-probe.exe" /link "%NGX_SDK%\lib\Windows_x86_64\x64\nvsdk_ngx_s.lib" advapi32.lib shell32.lib ole32.lib shlwapi.lib version.lib wintrust.lib crypt32.lib bcrypt.lib dxgi.lib d3d12.lib user32.lib
python tools\resistance-recorder\verify_nr_replay.py "%NR_PACKAGE%" "%NR_FRAMES%" "%NR_PACKAGE%"
```

For the DLAA control, include the same signed `nvngx_dlss.dll` used by the game in the offline probe directory. The model installer copies only the NR DLL into the game. The replay takes nine consecutive 3440×1440 Resistance frames, prepared as tightly packed RGBA8 display RGB, R32F reversed depth and RG32F pixel-space motion. This capture uses an offline display transform on raw DLAA output. Each run has fresh output, two preset logs, and pixel comparisons on successful evaluation. The model's own GPU time excludes capture and transfer. The live added-DLAA host still needs a verified display-color scene before the HUD on each API.
