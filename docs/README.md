# Storage Wiper documentation

Storage Wiper erases Linux storage devices through a GUI or CLI connected to a
privileged D-Bus helper. Start with a tutorial to learn the application without
erasing a device, or choose the documentation that matches your current task.

This documentation follows [Diátaxis](https://diataxis.fr/). Its four sections
serve different needs:

| Section | Use it when you want to… | Starting points |
| --- | --- | --- |
| [Tutorials](tutorials/README.md) | Learn through a guided exercise | [Build and explore](tutorials/build_and_explore.md), [inspect devices](tutorials/inspect_devices.md) |
| [How-to guides](how_to/README.md) | Complete a specific task | [Install](how_to/install.md), [wipe a device](how_to/wipe_device.md), [export a certificate](how_to/export_certificate.md) |
| [Reference](reference/README.md) | Look up commands, formats, and contracts | [CLI](reference/cli.md), [algorithms](reference/algorithms.md), [configuration](reference/configuration.md) |
| [Explanation](explanation/README.md) | Understand the design and its limits | [Architecture](explanation/architecture.md), [sanitization](explanation/sanitization.md), [device scope](explanation/device_scope.md) |

## Choose a route

- **New contributor:** [build and explore](tutorials/build_and_explore.md), then
  [run checks](how_to/run_checks.md) and read the
  [architecture](explanation/architecture.md).
- **New user:** [install](how_to/install.md), then
  [inspect devices](tutorials/inspect_devices.md) before following the
  [wipe guide](how_to/wipe_device.md).
- **Script author:** use the [CLI reference](reference/cli.md) and
  [certificate format](reference/configuration.md#certificates).
- **Integrator:** use the [D-Bus contract](reference/dbus.md),
  [build reference](reference/build.md), and
  [Arch packaging guide](how_to/package_archlinux.md).
- **Something went wrong:** use [troubleshooting](how_to/troubleshoot.md).

## Scope and conventions

These pages describe the implementation in this checkout. The
[build reference](reference/build.md) links to the version source; an installed
client reports its version with `storage-wiper-cli --version`.

Commands run as a normal user unless they explicitly use `sudo`. Destructive
examples appear in task guides and use a device placeholder that you must
replace after checking its identity. Tutorials use pipes, simulated devices,
or read-only inspection.

For documentation changes, follow
[write documentation](how_to/write_documentation.md). Return to the
[project overview](../README.md) for features, support, and licensing.
