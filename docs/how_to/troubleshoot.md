# Troubleshoot

Use the symptom below to narrow a failure. Read the logs before changing the
installation or restarting a helper; wait for active wipes to finish first.

## The build cannot find a library or C++ feature

1. Read the first failed compiler or dependency check in Meson's output and
   `meson-logs/meson-log.txt` inside your build directory.
2. Compare installed development packages with the
   [dependency list](../reference/build.md#dependencies).
3. For missing `std::format` or `std::expected`, check both the compiler and
   its C++ standard library. Selecting C++23 alone does not add missing library
   implementations.
4. After installing dependencies, reconfigure or create a separate fresh build.

The declared GTK/libadwaita minimum versions are Meson checks; the source uses
modern APIs such as `AdwAlertDialog`. Use development packages that provide the
actual APIs when an older distribution passes the version check but fails
compilation.

## The client cannot connect to the helper

1. Check that `storage-wiper-cli --version` works.
2. Confirm that the helper, system-bus activation file, bus policy, and polkit
   policy were installed, using the
   [installation paths](../reference/build.md#installed-files).
3. Check the system bus and inspect the helper interface:

   ```bash
   systemctl status dbus.service
   gdbus introspect --system --dest su.kidoz.storage_wiper.Helper \
     --object-path /su/kidoz/storage_wiper/Helper
   ```

4. If you use the systemd helper, inspect its unit and journal:

   ```bash
   systemctl status storage-wiper-helper.service
   journalctl -u storage-wiper-helper.service -b
   ```

5. Correct missing installation files and reload configuration using
   [install](install.md). A client built from a checkout still needs an installed
   helper for live device operations.

Direct D-Bus activation does not necessarily appear under the helper's systemd
unit. See [helper activation](../explanation/architecture.md#helper-activation).

## The client reports a helper from an older version

An upgrade replaces the helper executable, but not a helper process that is
already running: it keeps serving the protocol of the build it started with. A
client that was built later detects the mismatch and names both record types, for
example:

```text
storage-wiper-helper did not reply with a(sssxbbsbsubbxiiiiiiibs) (got a(sssxbbsbsubbxiiiiiii))
```

Clients that predate that check print a `GLib-CRITICAL` about a `GVariant`
format string and report no devices instead.

1. Stop the running helper; D-Bus activation starts the current build on the
   next client call:

   ```bash
   sudo pkill -f storage-wiper-helper
   ```

2. If the helper runs under the systemd unit, use
   `sudo systemctl restart storage-wiper-helper.service` instead. Only one
   process can own the helper bus name, so a new instance gives up while an
   older one still holds it.
3. Repeat the failed command. Listing devices does not write to a disk.
4. An upgrade stops an idle helper for you; a helper that was in the middle of
   an operation is deliberately left running. See
   [activate an upgraded helper](package_archlinux.md#activate-an-upgraded-helper).

## Authorization is denied or no password dialog appears

1. Run the GUI and CLI as your normal user in a session with an available polkit
   agent. A terminal-only session may lack a graphical authentication agent.
2. Check the installed policy and any administrator overrides against the
   [authorization reference](../reference/dbus.md#authorization).
3. Remember that `--yes` only skips the CLI confirmation. Wipe, unmount, and
   cancel operations still require their polkit authorization.
4. If a dialog is open, complete or dismiss it. Authorization checks are
   synchronous on the helper's main loop, so progress delivery can pause while
   a check waits.

## A device is missing or rejected

1. Compare the path with the supported families in
   [device scope](../explanation/device_scope.md).
2. Use the actual canonical block-device path. Arbitrary files, traversal paths,
   loop devices, and device-mapper volumes are not eligible.
3. Refresh the list after connecting or removing hardware. A device that
   disappeared, became mounted, or entered a conflicting operation can be
   rejected even if an earlier list showed it.
4. For an NVMe hardware erase, check every namespace on that controller. A
   mounted or busy sibling can prevent starting.

A missing SMART record is separate from device eligibility. Some USB bridges
do not support SMART pass-through, and SD cards do not provide the supported
health interface. Unknown health values are not a clean bill of health.

## Hardware erase fails or verification is unavailable

1. Read the helper error and device-specific requirements. ATA firmware erase
   can be unavailable on a security-frozen drive; NVMe requires supported
   Sanitize or cryptographic Format capabilities.
2. Check the [algorithm table](../reference/algorithms.md). Only Zero Fill,
   Random Fill, and DoD support the application's readback verification.
3. If `--verify` was requested for another algorithm, check the reported
   `verification_enabled` field; the helper disables unsupported verification.
4. Choose another operation only after evaluating its
   [sanitization limits](../explanation/sanitization.md). Firmware failure does
   not automatically start a software overwrite.

## Logs or certificates are missing

Read helper file logs, when present:

```bash
sudo tail -n 100 /var/log/storage-wiper/storage-wiper-helper.log
```

Read the appropriate client log:

```bash
tail -n 100 "${XDG_DATA_HOME:-$HOME/.local/share}/storage-wiper/logs/storage-wiper.log"
tail -n 100 "${XDG_DATA_HOME:-$HOME/.local/share}/storage-wiper/logs/storage-wiper-cli.log"
```

For a systemd-managed helper, also read its journal. The unit creates the helper
log directory; a failed file-log initialization falls back to console output.
See [logging locations](../reference/configuration.md#logging).

GUI certificates are written after a successful wipe; CLI certificates require
`--certificate`. Check the client log and destination permissions, then follow
[export a certificate](export_certificate.md). A successful CLI exit does not
guarantee that certificate files were written.

## Report a reproducible issue

Include the client version, distribution, compiler/build configuration when
relevant, exact command or GUI action, and the error text. Attach relevant logs
after removing device serials or other identifying data you do not want to
share. Prefer a reproduction using the project's temporary-file or simulated
fixtures. Submit it through [GitHub Issues](https://github.com/kidoz/storage-wiper/issues).

[How-to guides](README.md) · [Documentation home](../README.md)
