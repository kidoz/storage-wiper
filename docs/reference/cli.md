# Command-line interface

Executable: `storage-wiper-cli`. Source:
[CliApplication.cpp](../../src/cli/CliApplication.cpp).
Live device commands use the installed D-Bus helper; `--help` and `--version`
work without a helper connection.

## Commands and options

```text
storage-wiper-cli [OPTIONS]
```

| Short | Long | Argument | Behavior |
| --- | --- | --- | --- |
| `-h` | `--help` | None | Print help |
| `-V` | `--version` | None | Print the compiled project version |
| `-l` | `--list` | None | List eligible disks and partitions |
| `-j` | `--json` | None | Format `--list` as JSON |
| `-w` | `--wipe` | Device path | Start a wipe and wait for its result |
| `-a` | `--algorithm` | Name | Select an algorithm; default `zero-fill` |
| `-v` | `--verify` | None | Request readback verification when supported |
| `-f` | `--force-unmount` | None | Attempt to unmount the selected mounted target before wiping |
| `-y` | `--yes` | None | Skip the CLI's typed confirmation |
| `-c` | `--certificate` | Path | Write JSON and text records after a successful wipe |

There are no positional arguments. If both `--list` and `--wipe` are supplied,
the list command takes precedence. `--json` affects listing only; wipe progress
remains terminal output.

`--yes` does not bypass polkit or helper-side checks. `--force-unmount` does not
automatically unmount every namespace in an NVMe hardware erase's scope.
Unsupported `--verify` requests produce a warning and run without verification.
See [wipe a device](../how_to/wipe_device.md) for a complete procedure.

## Algorithm names

Names are case-insensitive. Use canonical names in scripts and documentation.

| Canonical name | Accepted aliases |
| --- | --- |
| `zero-fill` | `zero`, `zerofill` |
| `random-fill` | `random`, `randomfill` |
| `dod-5220-22-m` | `dod`, `dod522022m` |
| `gutmann` | None |
| `schneier` | None |
| `vsitr` | None |
| `gost` | `gost-r-50739-95` |
| `ata-secure-erase` | `hardware-secure-erase` |

The last name selects hardware erase for both ATA and NVMe devices. Pass counts,
numeric IDs, and verification support are in the
[algorithm reference](algorithms.md).

## Exit statuses

| Status | Meaning |
| --- | --- |
| `0` | Help/version printed, listing succeeded, or wipe reported success |
| `1` | Runtime failure, rejected/failed/cancelled wipe, declined confirmation, unknown algorithm, or no command |
| `2` | Argument parsing failure, such as an unknown option, missing required argument, or unexpected positional argument |

No command prints help after attempting to connect to the helper. An unknown
algorithm is a runtime error, not an option-parsing error. A successful wipe
with a certificate write failure still exits `0` and prints a warning. Success
can also include reported unwritable sectors; check the result record when
your workflow requires complete coverage.

## JSON device list

`storage-wiper-cli --list --json` writes an array to standard output. An empty
eligible-device list is `[]`. Sizes are bytes. Unavailable SMART numeric values
are JSON `null`.

A failed listing can also emit `[]` while exiting `1`. Scripts must check the
exit status to distinguish a failure from a successful empty list.

| Field | Type | Meaning |
| --- | --- | --- |
| `path` | String | Canonical device path |
| `model` | String | Model description |
| `size_bytes` | Integer | Device size |
| `is_ssd` | Boolean | Non-rotational device classification |
| `is_removable` | Boolean | Reported removable flag |
| `is_mounted` | Boolean | Reported mount state |
| `mount_point` | String | Reported mount location, or empty |
| `filesystem` | String | Reported filesystem type, or empty |
| `is_partition` | Boolean | Whether the entry represents a partition |
| `parent_disk` | String | Parent path for a partition; empty for a whole disk |
| `smart_status` | String | Health display label |
| `smart` | Object | Health attributes below |

The `smart` object contains:

| Field | Type |
| --- | --- |
| `available`, `healthy` | Boolean |
| `power_on_hours` | Integer or `null` |
| `temperature_celsius` | Integer or `null` |
| `reallocated_sectors`, `pending_sectors`, `uncorrectable_errors` | Integer or `null` |
| `percentage_used` | Integer or `null` |
| `available_spare_percent`, `available_spare_threshold_percent` | Integer or `null` |

The CLI JSON listing has no `serial` field. Serial numbers are present in
the D-Bus disk records and certificates. Partitions inherit the parent's SMART
record. Unknown health information does not establish that a device is healthy.

## Certificates and cancellation

`--certificate` accepts an existing directory or a path ending in `/` for an
automatically named pair. Otherwise it is a basename to which `.json` and `.txt`
are appended. See [certificate files](configuration.md#certificates).

While waiting for a wipe, SIGINT or SIGTERM requests cancellation for its device.
The helper reports the final result. Firmware work already accepted by hardware
may continue despite a cancellation request; see
[sanitization](../explanation/sanitization.md#firmware-erasure).

[Reference](README.md) · [Documentation home](../README.md)
