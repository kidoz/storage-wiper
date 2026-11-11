# Install the application and helper

Use this guide to install the GUI, CLI, and root helper from source on a Linux
system using D-Bus, polkit, and systemd. For Arch package installation, see
[package for Arch Linux](package_archlinux.md).

## Build for the system prefix

Install the [build dependencies](../reference/build.md#dependencies) and work
from the repository root. Keep this build separate from a development build.

```bash
meson setup build-install --prefix=/usr --libdir=lib --buildtype=release \
  -Dsystemd_system_unit_dir=/usr/lib/systemd/system
meson compile -C build-install
```

`/usr` places D-Bus activation, bus policy, and polkit files in the usual system
data directories. Change `libdir` and `systemd_system_unit_dir` if your
distribution uses different paths. A custom prefix also requires those system
services to discover the installed configuration; copying just the client
binary is insufficient.

For an already configured build directory, use `meson configure` to set options
before compiling. See [build options](../reference/build.md#options).

## Install and reload configuration

```bash
sudo meson install -C build-install
sudo systemctl daemon-reload
sudo systemctl reload dbus.service
```

This installs the three executables and desktop/service integration files.
Reloading configuration does not replace an already running helper. If you
are upgrading, let every active wipe finish before replacing or restarting
the helper; see [helper upgrades](package_archlinux.md#activate-an-upgraded-helper).

## Helper activation

On a systemd-enabled system bus, D-Bus starts `storage-wiper-helper.service` on
demand, including the unit's sandbox. Enabling it at boot is unnecessary.
Inspect the activated service with:

```bash
systemctl status storage-wiper-helper.service
journalctl -u storage-wiper-helper.service -b
```

An older directly activated helper can still own the bus name after upgrading.
Wait for every operation to finish before stopping it; the next client call
starts the new helper through systemd. Do not terminate a helper whose wipe
state is uncertain. Service shutdown waits for firmware operations that cannot
be cancelled, so stopping the unit may take as long as the device needs.

The [architecture explanation](../explanation/architecture.md#helper-activation)
describes activation and the fallback for buses without systemd support.

## Verify the installation

Run the clients as your normal user:

```bash
storage-wiper-cli --version
storage-wiper-cli --list
storage_wiper
```

The CLI should list eligible devices and the GUI should show the same devices.
An empty eligible-device list is a valid result. Follow
[inspect devices](../tutorials/inspect_devices.md) for a read-only walkthrough.

Use the binary directly or `just run-noroot` from a checkout. The current
`just run`, `run-debug`, `run-inspect`, and `run-pkexec` recipes elevate the GUI;
the intended client/helper separation uses an unprivileged GUI instead.

For installation paths, see [build reference](../reference/build.md#installed-files).
For failures, see [troubleshooting](troubleshoot.md).

[How-to guides](README.md) · [Documentation home](../README.md)
