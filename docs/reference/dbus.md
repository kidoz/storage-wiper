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
| `GetDiskSMART` | `s` path | `bbxiiiiiiiu` | `list-disks` |
| `ValidateDevicePath` | `s` path | `bs` valid, error message | `list-disks` |
| `IsDeviceWritable` | `s` path | `b` writable | `list-disks` |
| `UnmountDevice` | `s` path | `bs` success, error message | `wipe-disk` |
| `GetAlgorithms` | None | `a(ussi)` | `list-disks` |
| `StartWipe` | `sub` path, algorithm ID, verify | `bs` started, error message | `wipe-disk` |
| `CancelWipe` | `s` path | `b` cancelled | `wipe-disk` |

`StartWipe` acceptance is not completion; follow `WipeProgress` for that path.
The helper validates paths and rechecks operation eligibility before starting.
Unsupported verification is disabled and reported through progress.
`CancelWipe` targets one device and returns false when no operation runs there.

`GetAlgorithms` records contain ID, name, description, and pass count. IDs are
listed in the [algorithm reference](algorithms.md).

## Disk record

Each `GetDisks` element has 22 fields in this order:

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

## WipeProgress signal

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
sizes, and progress timing. Clients distinguish concurrent operations by path.

## Authorization

Source: [shipped polkit policy](../../data/su.kidoz.storage_wiper.policy).
Every method above performs a polkit check using the D-Bus caller's identity.

| Action | `allow_any` | `allow_inactive` | `allow_active` |
| --- | --- | --- | --- |
| `su.kidoz.storage_wiper.list-disks` | `auth_admin` | `auth_admin` | `yes` |
| `su.kidoz.storage_wiper.wipe-disk` | `auth_admin` | `auth_admin` | `auth_admin` |

Active local listing does not require administrator authentication by default.
Destructive actions use `auth_admin`, without the `keep` variant. Administrator
rules can override the shipped defaults. The helper's check allows user
interaction and currently waits synchronously on its main loop.

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
`WipeProgress` signal that does not match is ignored, and a `GetDisks` reply with
an unexpected type is reported as a named version skew showing both record types
instead of an empty device list. In practice such a reply comes from a helper
process left over from an earlier package version; see
[activate an upgraded helper](../how_to/package_archlinux.md#activate-an-upgraded-helper).

[Reference](README.md) · [Documentation home](../README.md)
