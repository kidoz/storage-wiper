# Build and explore

In this tutorial you will build Storage Wiper, inspect the CLI, and observe
algorithm tests using pipes and simulated devices. You will finish
with a working development build without installing a helper or wiping a disk.

## Before you start

Use Linux with Git, a C++23 compiler and standard library, Meson, Ninja,
pkg-config, GTK4/gtkmm/libadwaita and polkit development packages, and Google
Test/Mock. See the [dependency list](../reference/build.md#dependencies).

Run all commands as your normal user. The build directory in this exercise is
`build-tutorial`; start with a checkout that does not already contain that
configured directory.

## 1. Get the source

```bash
git clone https://github.com/kidoz/storage-wiper.git
cd storage-wiper
```

You are now at the repository root, alongside `meson.build`, `src/`, and
`tests/`. If you already have a checkout, enter its root instead.

## 2. Configure a debug build

```bash
meson setup build-tutorial --buildtype=debug -Denable_tests=true
```

Meson prints the compiler and dependency results and writes the build
configuration to `build-tutorial`. A missing dependency stops configuration;
install the named development package before continuing.

## 3. Compile

```bash
meson compile -C build-tutorial
```

The build produces `storage_wiper`, `storage-wiper-cli`, and
`storage-wiper-helper`, together with three test executables.

## 4. Inspect the CLI

```bash
./build-tutorial/storage-wiper-cli --version
./build-tutorial/storage-wiper-cli --help
```

The first command prints the version from this checkout. The second prints
commands and algorithm names. Neither command needs the system helper.

Find `--list`, `--wipe`, and `--verify` in the help output. In this exercise you
will use tests rather than the destructive `--wipe` command.

## 5. Observe a Zero Fill test

```bash
./build-tutorial/storage_wiper_tests --gtest_filter=ZeroFillAlgorithmTest.Execute_WritesOnlyZeros
```

Google Test reports one passing test. The fixture creates a pipe, executes the
real Zero Fill algorithm into its write end, and checks that the reader received
only zero bytes.

The test is defined in
[ZeroFillAlgorithmTest.cpp](../../tests/unit/algorithms/ZeroFillAlgorithmTest.cpp).
You have exercised algorithm code without selecting a `/dev/` device.

## 6. Run the simulated service tests

```bash
meson test -C build-tutorial device_io_tests dbus_client_tests --print-errorlogs
```

Meson reports two successful test suites. The device-I/O suite substitutes
registered fake devices for system calls. The D-Bus client suite starts a
private test bus. Neither suite requires installation of the system helper.

You now have a development build and have seen how the project exercises disk
operations without real disk writes. Continue with
[run tests and quality checks](../how_to/run_checks.md), or
[install the application](../how_to/install.md) before the
[device inspection tutorial](inspect_devices.md).

[Tutorials](README.md) · [Documentation home](../README.md)
