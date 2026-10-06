# Configuration and files

Storage Wiper stores GUI preferences, client logs, and wipe certificates under
the user's directories. The root helper has a separate log directory. There is
no general configuration file for the helper's algorithm or polkit behavior.

## GUI preferences

Source: [AppSettings.cpp](../../src/util/AppSettings.cpp),
[AppSettings.hpp](../../src/util/AppSettings.hpp), and
[Application.cpp](../../src/Application.cpp).

Path: `$XDG_CONFIG_HOME/storage-wiper/settings.conf`, falling back to
`$HOME/.config/storage-wiper/settings.conf` when `XDG_CONFIG_HOME` is empty or
unset.

The format is one `key=value` per line:

```ini
algorithm=0
verification=false
```

| Key | Values | Default | Behavior |
| --- | --- | --- | --- |
| `algorithm` | Numeric [algorithm ID](algorithms.md) | `0` | Parsed IDs are clamped to the supported range |
| `verification` | `true` or `1` enables; other values disable | `false` | Requested verification preference |

Unknown keys are ignored. Missing or unreadable files use defaults; malformed
algorithm values retain the current parsed/default value. Parsing does not trim
key whitespace, so use the shown format.

The GUI loads preferences on startup and saves them when a wipe starts. The
CLI takes its algorithm and verification choice from its own arguments.

## Logging

Source: [Logger.hpp](../../src/util/Logger.hpp),
[Logger.cpp](../../src/util/Logger.cpp), and the client/helper entry points.

| Process | Directory | Current file |
| --- | --- | --- |
| GUI | `$XDG_DATA_HOME/storage-wiper/logs` | `storage-wiper.log` |
| CLI | `$XDG_DATA_HOME/storage-wiper/logs` | `storage-wiper-cli.log` |
| Helper | `/var/log/storage-wiper` | `storage-wiper-helper.log` |

The client data-directory fallback is `$HOME/.local/share`. Log entries contain
timestamps, severity, and component tags. Default file rotation starts at
10 MiB and retains seven rotated files. These are implementation defaults, not
CLI flags or settings-file keys.

The [systemd unit](../../data/dbus/storage-wiper-helper.service.in) uses
`LogsDirectory=storage-wiper` with mode `0750`, allowing writes under its strict
filesystem protection. Standard output and error go to the journal for a
systemd-managed helper. If helper file logging cannot initialize, it enables
console output as a fallback. Direct D-Bus activation does not apply the unit's
directory management; see [helper activation](../explanation/architecture.md#helper-activation).

For commands to inspect logs, see [troubleshooting](../how_to/troubleshoot.md#logs-or-certificates-are-missing).

## Certificates

Source: [WipeCertificate.hpp](../../src/util/WipeCertificate.hpp) and
[WipeCertificate.cpp](../../src/util/WipeCertificate.cpp).

The GUI writes a JSON/text pair after each successful wipe to
`$XDG_DATA_HOME/storage-wiper/certificates`, falling back to
`$HOME/.local/share/storage-wiper/certificates`. Automatically generated
basenames include a UTC timestamp and the device basename.

The CLI writes only when `--certificate` is supplied. An existing directory,
or a path ending in `/`, gets an automatically named pair. Any other path is a
basename; the writer appends `.json` and `.txt`, creates parent directories,
and replaces existing files with the same names. The pair is not written
atomically, so a failure can leave only one file.

The JSON structure is:

| Field | Type | Meaning |
| --- | --- | --- |
| `type` | String | Literal `storage-wiper-certificate` |
| `tool_version` | String | Compiled project version |
| `success` | Boolean | Recorded operation result |
| `device.path`, `device.model`, `device.serial` | String | Device identity |
| `device.size_bytes` | Integer | Size in bytes |
| `device.is_partition` | Boolean | Partition selection |
| `device.parent_disk` | String | Parent path for a partition, otherwise empty |
| `wipe.algorithm`, `wipe.nist_category` | String | Algorithm name and application category label |
| `wipe.passes` | Integer | Algorithm pass-count metadata |
| `wipe.started_at`, `wipe.completed_at` | String | ISO 8601 UTC timestamps |
| `wipe.duration_seconds` | Integer | Observed elapsed seconds |
| `wipe.peak_speed_bytes_per_sec` | Integer | Maximum observed progress speed |
| `wipe.bad_block_count` | Integer | Final-pass unwritable-sector count |
| `verification.enabled` | Boolean | Whether supported verification ran |
| `verification.passed` | Boolean | Verification verdict when enabled |

When `verification.enabled` is false, `verification.passed: false` means there
is no successful readback verdict; it is not itself a reported verification
failure. `success: true` can accompany a nonzero bad-block count. Interpret both
alongside [verification and media limits](../explanation/sanitization.md).

Certificates are unsigned local records. The application has no certificate
history service or later export operation. See
[export a certificate](../how_to/export_certificate.md).

[Reference](README.md) · [Documentation home](../README.md)
