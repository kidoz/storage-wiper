# D-Bus interface

The GUI and CLI use the same privileged helper interface on the system bus.

| Item | Value |
| --- | --- |
| Bus | System |
| Service name | `su.kidoz.storage_wiper.Helper` |
| Object path | `/su/kidoz/storage_wiper/Helper` |
| Interface | `su.kidoz.storage_wiper.Helper` |

Sources: [published introspection XML](../../data/dbus/su.kidoz.storage_wiper.Helper.xml),
[shared signatures](../../src/services/DBusSignatures.hpp),
[helper handlers](../../src/helper/main.cpp), and
[DBusClient](../../src/services/DBusClient.cpp).

The tables below use D-Bus signatures: `s` string, `b` boolean, `u` unsigned
32-bit integer, `i` signed 32-bit integer, `t` unsigned 64-bit integer, `x` signed
64-bit integer, and `d` double. `a(...)` is an array of structures. Method
signatures list arguments in order; GVariant internally wraps complete calls
and replies in tuples.

## Methods

| Method | Input | Output | Authorization action suffix |
| --- | --- | --- | --- |
| `GetDisks` | None | `a(sssxbbsbsubbxiiiiiiibs)` | `list-disks` |
| `GetDisks2` | None | `ua(sssxbbsbsubbxiiiiiiibsbb)` version, records | `list-disks` |
| `GetDiskSMART` | `s` path | `bbxiiiiiiiu` | `list-disks` |
| `ValidateDevicePath` | `s` path | `bs` valid, error message | `list-disks` |
| `IsDeviceWritable` | `s` path | `b` writable | `list-disks` |
| `UnmountDevice` | `s` path | `bs` success, error message | `wipe-disk` |
| `GetAlgorithms` | None | `a(ussi)` | `list-disks` |
| `PrepareWipe` | `sub` path, algorithm ID, verify | `a{sv}` versioned plan | `wipe-disk` |
| `StartPreparedWipe` | `s` plan ID | `bs` started, error message | `wipe-disk` |
| `DiscardWipePlan` | `s` plan ID | `b` discarded | Sender ownership |
| `StartWipe` | `sub` path, algorithm ID, verify | `bs` always rejected | None |
| `GetWipeJobs` | None | `aa{sv}` retained job snapshots | `list-disks` |
| `CancelPreparedWipe` | `s` plan ID | `b` cancellation requested | `wipe-disk` |
| `CancelWipe` | `s` path | `b` always false | `wipe-disk` |

`StartPreparedWipe` acceptance is not completion; follow `PreparedWipeProgress`
for that plan ID. The helper validates paths and rechecks eligibility and kernel
device identity before writing. Unsupported verification is disabled in the
prepared plan and reported through progress. The path-only `StartWipe` remains
advertised for compatibility diagnostics but always rejects the operation.
Clients must upgrade; they must not fall back to path-only wiping.

`PrepareWipe` performs read-only discovery. The version-2 dictionary contains
`version` (u), `id` (s), `device` (identity tuple), `algorithm` (u), `verify` (b),
`method` (s), `scope` (s), `cancellable` (b), and `targets` (array of identity tuples).
An identity tuple has signature `(sssttttbsbbbb)`: path, model, serial, size,
kernel device number, kernel disk sequence, partition start in 512-byte sectors,
is-partition, parent path, is-SSD, is-removable, positive LVM physical-volume
signature flag, and active device-mapper holder flag. A whole-disk flag includes
its child partitions. Inactive physical volumes are detected from their metadata;
device-mapper holders are reported separately and do not by themselves prove LVM.
Scope is `device` or `nvme-subsystem`; method identifies the selected overwrite,
ATA erase, NVMe sanitize action, or NVMe cryptographic format.

Plans are owned by the helper and bound to the caller's unique D-Bus name.
They expire after two minutes, are consumed once, and are discarded on caller
disconnect. The helper retains at most two plans per caller and 32 globally.
The client displays the returned snapshot before confirming. Start transmits only
the ID; it cannot change the prepared device, method, scope, or verification.
Unknown plan versions and missing required fields fail closed.
`CancelPreparedWipe` checks that the UUID still identifies the current active job
for its path, that the prepared method is cancellable, and that the caller owns
the job. It returns false for finished, unknown, replaced, foreign, or
noncancellable jobs. The legacy path-only
`CancelWipe` always returns false; it cannot cancel a replacement job.

`GetAlgorithms` records contain ID, name, description, and pass count. IDs are
listed in the [algorithm reference](algorithms.md).

## Reconnect and retained job history

`GetWipeJobs` returns up to 64 job dictionaries. Each version-1 job dictionary
contains `version` (u), `plan` (a{sv}, the immutable prepared plan), `progress`
(the same tuple used by `PreparedWipeProgress`), `started-at` and `completed-at`
(x, Unix seconds), `verification-mismatches` (t), and `peak-speed` (t, the
maximum observed progress speed; a helper that predates the field omits it).
`completed-at` is zero until a terminal result. Reattached clients use the
original identity, times and peak speed for certificates rather than looking up
the current occupant of a device path.

Job UUIDs survive caller disconnect within the running helper process. Completed
jobs expire after one hour and may be evicted sooner to keep the 64-job bound.
Active jobs are never evicted; when all slots are active, new starts are rejected.
Job history is in memory: helper restart, power loss, or forced termination loses
it. Absence after a restart is not evidence that a firmware erase was cancelled
or completed; firmware may still be running independently of the process.

Clients subscribe to `PreparedWipeProgress` before requesting snapshots and retain
unmatched events by UUID while attaching callbacks. Terminal state must not regress
if a buffered older update arrives after a snapshot. Status queries use the
`list-disks` action so opening the GUI does not request destructive authorization.
Users allowed to list disks can also see retained job identity, progress, and times.
Starting or cancelling a job still requires `wipe-disk` authorization.

On SIGTERM or SIGINT the helper stops accepting new preparation, starts, and
unmounts, cancels pending authorizations, and requests cancellation only for
cancellable jobs. It keeps its main loop running to deliver terminal progress
and monitor noncancellable firmware operations before joining workers and exiting.
The systemd unit waits for this drain; stopping the service can therefore wait
for a long firmware operation. Forced termination forfeits monitoring and history.

## Disk record

Legacy `GetDisks` retains its 22-field record. `GetDisks2` first returns contract
version 2, then records with these fields followed by positions 23 and 24:

| Position | Field | Type |
| --- | --- | --- |
| 1 | `path` | `s` |
| 2 | `model` | `s` |
| 3 | `serial` | `s` |
| 4 | `size_bytes` | `x` |
| 5 | `is_removable` | `b` |
| 6 | `is_ssd` | `b` |
| 7 | `filesystem` | `s` |
| 8 | `is_mounted` | `b` |
| 9 | `mount_point` | `s` |
| 10 | `smart_status` | `u` |
| 11 | `smart_available` | `b` |
| 12 | `smart_healthy` | `b` |
| 13 | `power_on_hours` | `x` |
| 14 | `reallocated_sectors` | `i` |
| 15 | `pending_sectors` | `i` |
| 16 | `temperature_celsius` | `i` |
| 17 | `uncorrectable_errors` | `i` |
| 18 | `percentage_used` | `i` |
| 19 | `available_spare_percent` | `i` |
| 20 | `available_spare_threshold_percent` | `i` |
| 21 | `is_partition` | `b` |
| 22 | `parent_disk` | `s` |
| 23 (v2) | `is_lvm_pv` | `b` |
| 24 (v2) | `has_dm_holders` | `b` |

Partitions follow their parent disk and inherit model, serial, and SMART data.
Whole-disk records have an empty parent path. Unknown SMART numeric values are
`-1` on D-Bus, unlike the CLI JSON's `null`.

## SMART record

`GetDiskSMART` returns, in order:

| Position | Field | Type |
| --- | --- | --- |
| 1 | `available` | `b` |
| 2 | `healthy` | `b` |
| 3 | `power_on_hours` | `x` |
| 4 | `reallocated_sectors` | `i` |
| 5 | `pending_sectors` | `i` |
| 6 | `temperature_celsius` | `i` |
| 7 | `uncorrectable_errors` | `i` |
| 8 | `percentage_used` | `i` |
| 9 | `available_spare_percent` | `i` |
| 10 | `available_spare_threshold_percent` | `i` |
| 11 | `status` | `u` |

Health status values are `0` unknown, `1` good, `2` warning, and `3` critical.
Unknown numeric attributes are `-1`.

## Health classification

The helper derives the status from the device's own verdict and from the
attributes it could read. Attributes that are absent or unknown (`-1`) never
raise a status.

A device is `critical` when it reports a failure verdict, when an NVMe critical
warning bit is set, when available spare capacity is below the vendor threshold,
or when a critical band is crossed: 50 reallocated sectors, 10 pending sectors,
life used at 100 percent, or temperature at 60 C for ATA and eMMC devices and
80 C for NVMe devices.

It is `warning` when a lower band is crossed instead: 5 reallocated sectors,
1 pending sector, any uncorrectable error, life used at 90 percent, or
temperature at 50 C for ATA and eMMC devices and 70 C for NVMe devices. Spare
capacity within 10 points of the vendor threshold is also a `warning`, ahead of
the drive's own critical warning. Otherwise the status is `good`.

The temperature bands differ by device class because an NVMe module idles near
50 C where a hard drive or a 2.5" SATA SSD is already hot. The bands are
advisory; an NVMe controller's own thermal verdict still arrives through its
critical warning bits.

## PreparedWipeProgress signal

The new signal contains a plan ID (`s`) followed by the progress tuple below:
`(s(sdiisbbstttxbbbdt))`. Clients match both plan ID and path. A delayed signal
from an earlier operation cannot complete a newer operation on the same path.
The legacy `WipeProgress` remains in introspection but is not emitted for
prepared operations.

### Progress tuple

Signature: `sdiisbbstttxbbbdt`, represented internally as
`(sdiisbbstttxbbbdt)`. Its 17 fields are:

| Position | Field | Type |
| --- | --- | --- |
| 1 | `device_path` | `s` |
| 2 | `percentage` | `d` |
| 3 | `current_pass` | `i` |
| 4 | `total_passes` | `i` |
| 5 | `status` | `s` |
| 6 | `is_complete` | `b` |
| 7 | `has_error` | `b` |
| 8 | `error_message` | `s` |
| 9 | `bytes_written` | `t` |
| 10 | `total_bytes` | `t` |
| 11 | `speed_bytes_per_sec` | `t` |
| 12 | `estimated_seconds_remaining` | `x` |
| 13 | `verification_enabled` | `b` |
| 14 | `verification_in_progress` | `b` |
| 15 | `verification_passed` | `b` |
| 16 | `verification_percentage` | `d` |
| 17 | `bad_block_count` | `t` |

ETA is `-1` when unknown. Verification percentage is separate from write
percentage. The terminal bad-block count represents unwritable sectors in the
final pass, not a sum across passes. The in-process `WipeProgress` model's
`verification_mismatches` field is not transmitted by this signal.

Signals are broadcast on the system bus, rather than addressed only to the
requesting client. Local users able to observe the bus can see device paths,
sizes, and progress timing. Clients distinguish concurrent operations by plan ID and path.

## Authorization

Source: [shipped polkit policy](../../data/su.kidoz.storage_wiper.policy).
Discovery and operation methods check polkit using the caller's identity.
Discard checks plan ownership; legacy StartWipe always rejects and legacy
CancelWipe never cancels a job.

| Action | `allow_any` | `allow_inactive` | `allow_active` |
| --- | --- | --- | --- |
| `su.kidoz.storage_wiper.list-disks` | `auth_admin` | `auth_admin` | `yes` |
| `su.kidoz.storage_wiper.wipe-disk` | `auth_admin` | `auth_admin` | `auth_admin` |

Active local listing does not require administrator authentication by default.
Destructive actions use `auth_admin`, without the `keep` variant. Administrator
rules can override the shipped defaults. Authorization runs asynchronously,
with a 20-second deadline, caller-disconnect cancellation, and limits of two
pending requests per sender and 16 globally. The main loop remains available
for progress while authentication is pending.

## Inspect the live contract

This command inspects the interface and can activate the helper; it does not
start a wipe:

```bash
gdbus introspect --system --dest su.kidoz.storage_wiper.Helper \
  --object-path /su/kidoz/storage_wiper/Helper
```

Compiled introspection uses the shared signature constants; regression tests
also check the published XML. When changing the contract, update the helper,
client, constants, published XML, and related tests together.

The client checks reply types against those constants before parsing. A
`PreparedWipeProgress` signal that does not match is ignored, and a `GetDisks2`
reply with an unexpected type is reported as a named version skew showing both
record types instead of an empty device list. An older helper that does not
implement `GetDisks2` at all is reported as an older running helper. In practice
such a reply comes from a helper process left over from an earlier package
version; see
[activate an upgraded helper](../how_to/package_archlinux.md#activate-an-upgraded-helper).

[Reference](README.md) · [Documentation home](../README.md)
