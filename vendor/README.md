# vendor/

This folder holds the optional third-party sensor resources that are embedded into
`ResMon.exe` at build time. The binaries are **not** committed to this repository.

They are downloaded on demand by `FETCH_SENSOR_DEPS.ps1` (run automatically by
`BUILD_AND_RUN.cmd`):

| File                | Source                                                      | Pinned to |
| ------------------- | ----------------------------------------------------------- | --------- |
| `PawnIO_setup.exe`  | [PawnIO.Setup releases](https://github.com/namazso/PawnIO.Setup/releases) | 2.2.0 (SHA-256 verified) |
| `IntelMSR.bin`      | [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor) | `v0.9.6` |
| `AMDFamily17.bin`   | [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor) | `v0.9.6` |

See [`../THIRD_PARTY.md`](../THIRD_PARTY.md) for licensing details.

CMake fails with a clear message if these files are missing, so run
`FETCH_SENSOR_DEPS.ps1` (or `BUILD_AND_RUN.cmd`) before configuring manually.
