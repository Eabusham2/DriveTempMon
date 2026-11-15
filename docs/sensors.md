# Drive temperature sensing

DriveTempMon reads physical-disk temperatures through Windows storage IOCTLs. It
does not shell out to PowerShell, WMI, `smartctl`, or a vendor utility. It does not
load its own kernel driver, run SMART self-tests, change drive SMART settings,
write SMART logs, set firmware temperature thresholds, or read user files.

## Supported paths

| Drive/controller path | Temperature request |
| --- | --- |
| Native NVMe with Windows protocol-query support | SMART/Health log page 02h; composite temperature |
| Windows storage driver exposing a temperature descriptor | Device temperature property; composite sensor 0, or hottest available sensor if no composite is reported |
| SATA/ATA | SMART READ DATA; conventional raw Celsius attribute 194, then 190 |
| Older Windows ATA SMART driver | `SMART_RCV_DRIVE_DATA`, SMART READ DATA |
| USB SATA bridge implementing SAT | ATA PASS THROUGH (16), then (12), SMART READ DATA |
| SCSI/SAS exposing the standard temperature log | LOG SENSE page 0Dh, current-temperature parameter 0000h |

NVMe protocol queries and ATA SMART reads return a 512-byte log because those
protocols do not provide a separate one-byte SMART temperature operation. Only
temperature is interpreted and retained. Windows temperature-property queries
request temperature data directly. Successful transports are cached; a normal
poll uses that one transport instead of rediscovering support each time. An
unsupported device is reprobed at most every five minutes, apart from explicit
device rediscovery. A fast poll targets only the requested hot drive IDs.

Device enumeration uses SetupAPI's disk-interface class and the storage device
number, rather than trying every possible `PhysicalDrive` number. Device names
come from the storage descriptor or Windows device properties. SSD/HDD detection
uses NVMe bus type or the Windows seek-penalty descriptor; an unreported kind
remains unknown. Model plus serial form a stable ID when the driver exposes a
serial; otherwise Windows' device instance ID is used. Settings cannot follow a
serial-less USB drive across a port/bridge change if Windows assigns it a new ID.

## Power, permissions, and unsupported hardware

Before each temperature poll, `GetDevicePowerState` is checked. If Windows says
the device is asleep, the temperature request is skipped and the UI reports
sleeping/unavailable instead of presenting an old value as a fresh reading. No
wake, START STOP UNIT, SMART ENABLE, or power-mode-changing command is sent.
Some drivers and USB bridges do not report power state accurately; Windows does
not provide a universal guarantee that a monitoring read will never wake such
hardware. Discovery metadata may also be obtained by the storage stack from the
device. This is a hardware/driver limitation, not an advertised zero-I/O claim.

ATA and SCSI pass-through need a handle with read/write access even though the
commands issued here are read-only. Windows normally requires administrator
access for that physical-disk handle. If access is unavailable, DriveTempMon
still attempts the Windows/NVMe queries that the driver's permissions allow and
reports when administrator access may be necessary. It never labels an unknown
temperature as healthy.

RAID/VMD controllers, proprietary USB NVMe enclosures, virtual disks, unusual
vendor SMART layouts, and controllers that hide or block these commands may
show unavailable. This release does not implement proprietary RAID or USB NVMe
vendor commands. SMART attributes are vendor defined; only the conventional
194/190 raw Celsius layout is supported. Drive firmware must already expose
SMART; the app deliberately does not enable it.

ATA/SAT/SCSI commands request a two-second device-command timeout. IOCTLs run on
the app's sensor worker, never the GUI thread. Driver-level property and legacy
SMART requests do not expose a per-call timeout parameter; a defective kernel
driver can exceed normal timing. Polling is best effort and not a real-time
temperature-control or data-loss-prevention guarantee.

## Validation

The parser test suite checks actual response layouts, truncated buffers,
malformed sizes/counts, invalid temperatures, SMART checksum failures, raw
versus normalized ATA values, composite NVMe temperatures, and the SCSI current
temperature parameter. These fixtures establish decoding behavior. Real drive
and bridge compatibility must also be checked on physical Windows hardware;
CI virtual drives do not prove that coverage.

## Primary API references

- [Working with NVMe drives](https://learn.microsoft.com/en-us/windows/win32/fileio/working-with-nvme-devices)
- [STORAGE_TEMPERATURE_DATA_DESCRIPTOR](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-storage_temperature_data_descriptor)
- [STORAGE_TEMPERATURE_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-storage_temperature_info)
- [ATA_PASS_THROUGH_EX and command timeout](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddscsi/ns-ntddscsi-_ata_pass_through_ex)
- [SMART_RCV_DRIVE_DATA and SENDCMDINPARAMS](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntdddisk/ns-ntdddisk-_sendcmdinparams)
- [SCSI pass-through, synchronous access and permissions](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddscsi/ni-ntddscsi-ioctl_scsi_pass_through)
- [GetDevicePowerState](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getdevicepowerstate)
- [SetupDiEnumDeviceInterfaces](https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupdienumdeviceinterfaces)
- [DEVICE_SEEK_PENALTY_DESCRIPTOR](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-device_seek_penalty_descriptor)
