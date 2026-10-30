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
| TPM | TBS (`TPM2_GetCapability`, `TPM2_ReadPublic`), Platform Crypto Provider, registry | Manufacturer, firmware, EK public key hash through each path, EK certificate, SRK / Windows AIK hashes |
| Disks | `IOCTL_STORAGE_QUERY_PROPERTY`, `SMART_RCV_DRIVE_DATA`, SCSI / ATA pass-through, NVMe Identify | Model, serial through each path, WWN, firmware, capacity |
| Volumes | `GetVolumeInformation` | Volume serial and GUID |
| Network | IP Helper, NDIS OID, registry | Current / permanent / overridden MAC |
| Monitors | EDID (SetupAPI) | Manufacturer, model, serial, ID serial, refresh rate |
| Graphics | DXGI, NVML, Vulkan | Vendor / device / subsystem IDs, GPU UUID (NVML for NVIDIA, Vulkan for any vendor), VRAM |
| Windows | Registry | MachineGuid, ProductId, HwProfileGuid, SQM MachineId, SusClientId |

Everything runs in user mode, no driver.

TPM key hashes are the SHA-256 of the RSA public key (DER), the same value as `PublicKeyHash` in `Get-TpmEndorsementKeyInfo -HashAlgorithm sha256`.

## Features

- Click any value to copy it to the clipboard.
- Export the full report to `.txt` or `.json`.
- Headless export: `SerialChecker.exe --export report.json` (or `.txt`).
- **Check (compare)**: highlights in green every value that changed since an export.
  1. Click **Export JSON** (saved on the Desktop by default).
  2. Later, click **Check**: the app re-reads the hardware and compares it with the newest `serials_*.json` on the Desktop. Changed values turn green with the old value struck through, new fields are tagged `NEW`, and the sidebar shows how many changes each section has.
  3. **Shift+Check** lets you pick another export file; **Clear** hides the comparison.
  4. Also available from the command line: `SerialChecker.exe --compare baseline.json`.

## Build

Open `SerialChecker.sln` in Visual Studio 2026 (toolset v145) and build `Release | x64`. Dear ImGui 1.92.9 is vendored in `third_party/imgui`, no other dependency.

```
msbuild SerialChecker.sln /p:Configuration=Release /p:Platform=x64
```
