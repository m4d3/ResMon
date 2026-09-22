# Third-party sensor components

ResMon itself is MIT licensed. The enhanced-temperature build fetches and embeds these upstream components at build time:

## PawnIO 2.2.0

- Project: https://github.com/namazso/PawnIO
- Setup releases: https://github.com/namazso/PawnIO.Setup/releases/tag/2.2.0
- Installer: `PawnIO_setup.exe`
- Expected SHA-256: `1F519A22E47187F70A1379A48CA604981C4FCF694F4E65B734AAA74A9FBA3032`
- Purpose: signed Windows kernel driver used for restricted low-level hardware access.
- License: see the upstream PawnIO repository. PawnIO is GPL-2.0 with its documented exception for independent modules that communicate solely through the device I/O-control interface.

ResMon communicates with PawnIO through the device I/O-control interface; it does not link against PawnIOLib.

## PawnIO modules from LibreHardwareMonitor v0.9.6

- Project: https://github.com/LibreHardwareMonitor/LibreHardwareMonitor
- Version/tag: `v0.9.6`
- Files used:
  - `LibreHardwareMonitorLib/Resources/PawnIo/IntelMSR.bin`
  - `LibreHardwareMonitorLib/Resources/PawnIo/AMDFamily17.bin`
- Purpose: restricted PawnIO modules for Intel MSR and AMD Zen MSR/SMN access.
- The PawnIO module source originates from the PawnIO.Modules project and is licensed under LGPL-2.1-or-later; see the upstream source headers/licenses.

ResMon does not embed or use `LibreHardwareMonitorLib.dll` and does not require .NET.

If you redistribute a ResMon executable containing these embedded components, review the exact upstream license terms and provide the corresponding notices/source access required by those licenses.


## NVIDIA NVML (runtime, not redistributed)

- Documentation: https://docs.nvidia.com/deploy/nvml-api/latest/
- Purpose: optional NVIDIA GPU die-temperature reading.
- ResMon uses `LoadLibrary`/`GetProcAddress` at runtime and loads `nvml.dll` from the installed NVIDIA driver when available.
- No NVIDIA DLL, SDK header, CUDA runtime, or `nvidia-smi` binary is copied into or redistributed with ResMon.
- If no compatible NVIDIA driver/NVML library is installed, this backend simply remains unavailable.
