# Build reference

Sources: [meson.build](../../meson.build),
[meson_options.txt](../../meson_options.txt), and [justfile](../../justfile).
The project version is declared in `meson.build` and compiled into clients and
certificates.

## Dependencies

| Requirement | Purpose |
| --- | --- |
| Linux | Block devices, sysfs, mount information, and device ioctls |
| C++23 compiler and standard library | `std::format`, `std::expected`, ranges, and other language/library features |
| Meson ≥ 0.59.0, Ninja, pkg-config | Configure, compile, and locate libraries |
| `gtk4` ≥ 4.0 | GTK GUI |
| `gtkmm-4.0` ≥ 4.6 | C++ GTK bindings |
| `libadwaita-1` ≥ 1.0 | Adwaita GUI components |
| `polkit-gobject-1` | Helper authorization |
| `gio-2.0` | D-Bus and GLib integration |
| D-Bus system bus and polkit service/agent | Installed device operations and interactive authorization |
| `gtest`, `gmock` | Builds with `enable_tests=true` |

Library names are pkg-config dependency names; install the corresponding
development packages on distributions that separate them. The listed GUI
minimums are the checks declared by Meson. The source uses newer APIs, including
`AdwAlertDialog`, so older packages that pass those minimum checks may still lack
required declarations. Use a recent GTK/libadwaita development stack.

Optional development tools: `just`, clang-format, clang-tidy, cppcheck,
scan-build, Valgrind, and `entr` for the watch recipe.

## Options

Project-specific options:

| Option | Type | Default | Effect |
| --- | --- | --- | --- |
| `enable_tests` | Boolean | `false` | Build and register all three test suites |
| `enable_clang_tidy` | Boolean | `false` | Add the `clang-tidy` target when the tool is found |
| `enable_cppcheck` | Boolean | `false` | Add the `cppcheck` target when the tool is found |
| `systemd_system_unit_dir` | String | Empty | Override the unit installation directory; default is `<prefix>/<libdir>/systemd/system` |

Relevant built-in Meson options:

| Option | Project default or example | Effect |
| --- | --- | --- |
| `cpp_std` | `c++23` | C++ standard |
| `buildtype` | `release` | Build configuration; use `debug` for development |
| `warning_level` | `3` | Compiler warnings |
| `werror` | `true` | Treat compiler warnings as errors |
| `optimization` | `3` | Default optimization setting |
| `b_ndebug` | `if-release` | Disable assertions in release builds |
| `prefix`, `bindir`, `libdir`, `datadir` | Distribution or Meson defaults | Installation paths |
| `b_sanitize` | `address`, `undefined`, `address,undefined`, or `thread` | Sanitizer selection |
| `b_lundef` | Set `false` in sanitizer examples | Linker undefined-symbol checking |

Use `meson configure BUILD_DIR` to inspect the actual configured values.
See [build and explore](../tutorials/build_and_explore.md) for a first build,
[install](../how_to/install.md) for system integration, and
[run checks](../how_to/run_checks.md) for validation commands.

## Executable targets

| Target | Purpose | Installed directory |
| --- | --- | --- |
| `storage_wiper` | Unprivileged GUI | `<prefix>/<bindir>` |
| `storage-wiper-cli` | Unprivileged CLI | `<prefix>/<bindir>` |
| `storage-wiper-helper` | Root device service | `<prefix>/<libdir>/storage-wiper` |

All three are built by default. There is no project option that builds only
the CLI without configuring the GUI dependencies.

## Test targets

With `enable_tests=true`:

| Meson test name | Executable | Coverage | Timeout |
| --- | --- | --- | --- |
| `unit_tests` | `storage_wiper_tests` | Algorithms on temporary files/pipes, ViewModel, services, utilities, and DI | 300 seconds |
| `device_io_tests` | `storage_wiper_io_tests` | Real wipe service and algorithms against wrapped fake device I/O | 60 seconds |
| `dbus_client_tests` | `storage_wiper_dbus_tests` | D-Bus client on a private test bus | 60 seconds |

`meson test -C BUILD_DIR` runs all registered suites. Google Test case filters
apply when invoking an individual executable. Tests do not need a root helper
installation; some unit cases can inspect real device metadata without writing
devices.

## Installed files

Directories below are relative to `prefix`, unless an absolute override is
configured:

| Artifact | Directory |
| --- | --- |
| GUI and CLI | `bindir` |
| Helper | `libdir/storage-wiper` |
| D-Bus activation file | `datadir/dbus-1/system-services` |
| D-Bus bus policy | `datadir/dbus-1/system.d` |
| Polkit policy | `datadir/polkit-1/actions` |
| Desktop entry | `datadir/applications` |
| AppStream metadata | `datadir/metainfo` |
| Application icon | `datadir/icons/hicolor/scalable/apps` |
| Symbolic icon | `datadir/icons/hicolor/symbolic/apps` |
| Systemd helper unit | `libdir/systemd/system`, or `systemd_system_unit_dir` |

The activation file and systemd unit embed the configured helper path. A custom
prefix does not automatically make D-Bus or polkit search its data directories.
`DESTDIR` staging changes the installation destination, not those embedded
runtime paths.

## Convenience recipes

Run `just` to list all recipes. Its default `build_dir` is `build`; override it
with a variable assignment, for example:

```bash
just build_dir=build-dev build-debug
```

| Task | Recipes |
| --- | --- |
| Build | `build`, `build-debug`, `rebuild`, `clean`, `build-reset` |
| Run an unprivileged GUI | `run-noroot` |
| Test | `test`, `test-verbose`, `test-filter PATTERN`, `test-list` |
| Format and analyze | `format`, `format-check`, `lint`, `lint-parallel`, `lint-fix`, `cppcheck`, `scan-build`, `analyze` |
| Memory checks | `valgrind`, `valgrind-quick`, `valgrind-report`, `valgrind-filter PATTERN`, `valgrind-gen-suppressions` |
| Sanitizers | `build-asan`, `build-ubsan`, `build-sanitizers`, `build-tsan`, `test-asan`, `test-ubsan`, `test-sanitizers` |
| CLI | `build-cli`, `cli-list`, `cli-list-json`, `cli-help` |
| System install | `install`, `uninstall` |
| Arch packaging | `pkg-arch`, `pkg-arch-install`, `pkg-arch-git`, `pkg-arch-git-install`, `pkg-arch-clean` |
| Logs | `logs-helper`, `logs-gui` |

`build-cli` depends on the normal build. Some sanitizer recipes use fixed
directory names; inspect the [justfile](../../justfile) before relying on a
`build_dir` override. The `run`, `run-debug`, `run-inspect`, and `run-pkexec`
recipes currently elevate the GUI. Use the client binary directly as a normal
user for the intended privilege separation.

[Reference](README.md) · [Documentation home](../README.md)
