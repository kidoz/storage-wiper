# Run tests and quality checks

Use these commands from the repository root to validate a change. Install the
[test and analysis dependencies](../reference/build.md#dependencies) for the
checks you choose. Run tests as a normal user.

## Run all test suites

For a new build directory:

```bash
meson setup build-checks --buildtype=debug -Denable_tests=true
meson compile -C build-checks
meson test -C build-checks --print-errorlogs
```

For an existing build, enable tests with
`meson configure build-checks -Denable_tests=true`, then compile and test.
Meson runs the unit, simulated device-I/O, and private-bus D-Bus suites. See
[test targets](../reference/build.md#test-targets) for their scope.

The command-runner equivalent is `just test`, which configures tests in its
selected build directory, builds, and runs all three suites.

## Focus a regression check

```bash
./build-checks/storage_wiper_tests --gtest_list_tests
./build-checks/storage_wiper_tests --gtest_filter='*MainViewModel*'
meson test -C build-checks device_io_tests dbus_client_tests --print-errorlogs
```

Google Test filtering applies to the executable you invoke. `just test-filter`
and `just test-list` use the unit executable; they do not select cases from the
separate device-I/O or D-Bus executables.

Do not replace simulated fixtures with real device paths. Algorithm unit tests
use temporary files; the device-I/O tests reject unregistered `/dev/` paths.
Some disk-service unit tests inspect real device metadata, so tests do not
require that the host have any particular physical disk.

## Check formatting

```bash
just format-check
```

Use `just format` to apply formatting before reviewing the diff. Both recipes
cover source and tests using the repository's clang-format configuration.

## Run static analysis

For a fresh analysis build:

```bash
meson setup build-analysis --buildtype=debug \
  -Denable_clang_tidy=true -Denable_cppcheck=true
meson compile -C build-analysis clang-tidy
meson compile -C build-analysis cppcheck
```

The tools must be installed for their targets to exist. Inspect analyzer
diagnostics as well as command status. For strict automation, invoke the tools
or Meson targets directly: some convenience `just` recipes print a fallback
message when a tool fails or is absent.

## Run sanitizers

Use a separate build directory so sanitizer options do not alter your regular
build:

```bash
meson setup build-sanitized --buildtype=debug -Denable_tests=true \
  -Db_sanitize=address,undefined -Db_lundef=false
meson compile -C build-sanitized
meson test -C build-sanitized --print-errorlogs
```

ThreadSanitizer requires its own build with `-Db_sanitize=thread`. Do not combine
it with AddressSanitizer. See the [build reference](../reference/build.md#options)
for built-in options and the existing sanitizer recipes.

## Run Valgrind

With a debug build and Valgrind installed:

```bash
valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 \
  ./build-checks/storage_wiper_tests --gtest_filter='*MainViewModel*'
```

The `just valgrind*` recipes also work without a suppressions file. They use
`.valgrind-suppressions` when it exists; `just valgrind-gen-suppressions`
generates a report to review when investigating third-party allocations.

See [troubleshooting](troubleshoot.md) for build failures. When updating user
behavior, also follow [write documentation](write_documentation.md).

[How-to guides](README.md) · [Documentation home](../README.md)
