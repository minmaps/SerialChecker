# Serial Checker

Minimal Windows app (C++ / Dear ImGui / Direct3D 11) that shows every hardware identifier of the machine in a clean black & white interface.

## Download

Grab `SerialChecker.exe` from the [latest release](../../releases/latest). It asks for administrator rights (needed for SMART / SCSI / ATA pass-through reads).

## What it reads

| Section | Source | Values |
|---|---|---|
| System, BIOS | SMBIOS (`GetSystemFirmwareTable`) | Manufacturer, product, serial, UUID, SKU, family, BIOS version |
| Baseboard, Chassis | SMBIOS | Manufacturer, product, serial, asset tag |
| Processor | SMBIOS | Model, Processor ID, serial, part number, socket |
| Memory | SMBIOS type 17 | Part number, serial, size, speed per module |
| Disks | `IOCTL_STORAGE_QUERY_PROPERTY`, `SMART_RCV_DRIVE_DATA`, SCSI / ATA pass-through, NVMe Identify | Model, serial through each path, WWN, firmware, capacity |
| Volumes | `GetVolumeInformation` | Volume serial and GUID |
| Network | IP Helper, NDIS OID, registry | Current / permanent / overridden MAC |
| Monitors | EDID (SetupAPI) | Manufacturer, model, serial, ID serial, refresh rate |
| Graphics | DXGI | Vendor / device / subsystem IDs, VRAM |
| Windows | Registry | MachineGuid, ProductId, HwProfileGuid, SQM MachineId, SusClientId |

Everything runs in user mode, no driver.

## Features

- Click any value to copy it to the clipboard.
- Export the full report to `.txt` or `.json`.
- Headless export: `SerialChecker.exe --export report.json` (or `.txt`).

## Build

Open `SerialChecker.sln` in Visual Studio 2026 (toolset v145) and build `Release | x64`. Dear ImGui 1.92.9 is vendored in `third_party/imgui`, no other dependency.

```
msbuild SerialChecker.sln /p:Configuration=Release /p:Platform=x64
```
