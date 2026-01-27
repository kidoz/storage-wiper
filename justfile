# Storage Wiper Development Commands
# Usage: just <command>

# Default recipe - show available commands
build_dir := "build"
default:
    @just --list

# === Build Commands ===

# Configure and build (release)
build:
    @if [ ! -d {{build_dir}} ]; then meson setup {{build_dir}}; fi
    @meson setup {{build_dir}} --reconfigure -Dbuildtype=release >/dev/null
    meson compile -C {{build_dir}}

# Configure and build (debug)
build-debug:
    @if [ ! -d {{build_dir}} ]; then meson setup {{build_dir}}; fi
    @meson setup {{build_dir}} --reconfigure -Dbuildtype=debug >/dev/null
    meson compile -C {{build_dir}}

# Clean build directory and rebuild
rebuild:
    rm -rf {{build_dir}}
    meson setup {{build_dir}}
    meson compile -C {{build_dir}}

# Clean all build artifacts
clean:
    rm -rf {{build_dir}}

# === Run Commands ===

# Run application (requires root for disk access, preserves DBUS session)
run: build
    sudo -E DBUS_SESSION_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS" ./{{build_dir}}/storage_wiper

# Run debug build
run-debug: build-debug
    sudo -E DBUS_SESSION_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS" ./{{build_dir}}/storage_wiper

# Run with GTK inspector enabled
run-inspect: build
    GTK_DEBUG=interactive sudo -E DBUS_SESSION_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS" ./{{build_dir}}/storage_wiper

# Run without root (limited functionality - can view disks but not wipe)
run-noroot: build
    ./{{build_dir}}/storage_wiper

# Run via pkexec (production-like, shows auth dialog)
run-pkexec: build
    pkexec ./{{build_dir}}/storage_wiper

# === Development Commands ===

# Watch for changes and rebuild (requires entr)
watch:
    find src -name '*.cpp' -o -name '*.hpp' | entr -c just build

# Check for compiler warnings
check: build
    meson compile -C {{build_dir}} 2>&1 | grep -E "warning:|error:" || echo "No warnings or errors"

# Format code (requires clang-format)
format:
    find src tests -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i

# Check formatting without modifying files
format-check:
    @find src tests -name '*.cpp' -o -name '*.hpp' | xargs clang-format --dry-run -Werror 2>&1 \
        && echo "All files formatted correctly" \
        || (echo "Run 'just format' to fix formatting"; exit 1)

# === Static Analysis Commands ===

# Run clang-tidy on all sources (requires clang-tidy)
lint:
    @if [ ! -d {{build_dir}} ]; then meson setup {{build_dir}} -Denable_clang_tidy=true; fi
    @meson setup {{build_dir}} --reconfigure -Denable_clang_tidy=true >/dev/null
    meson compile -C {{build_dir}} clang-tidy || echo "clang-tidy not available"

# Run clang-tidy with run-clang-tidy for parallel analysis (faster)
lint-parallel:
    @if [ ! -d {{build_dir}} ]; then meson setup {{build_dir}}; fi
    @if command -v run-clang-tidy >/dev/null 2>&1; then \
        run-clang-tidy -p {{build_dir}} -header-filter='.*' src/; \
    else \
        echo "run-clang-tidy not found. Install clang-tools-extra package."; \
    fi

# Run clang-tidy and fix issues automatically
lint-fix:
    @if [ ! -d {{build_dir}} ]; then meson setup {{build_dir}}; fi
    @if command -v run-clang-tidy >/dev/null 2>&1; then \
        run-clang-tidy -p {{build_dir}} -header-filter='.*' -fix src/; \
    else \
        echo "run-clang-tidy not found. Install clang-tools-extra package."; \
    fi

# Run cppcheck (if available)
cppcheck:
    @if [ ! -d {{build_dir}} ]; then meson setup {{build_dir}} -Denable_cppcheck=true; fi
    @meson setup {{build_dir}} --reconfigure -Denable_cppcheck=true >/dev/null
    meson compile -C {{build_dir}} cppcheck || echo "cppcheck not available"

# Run Clang Static Analyzer (scan-build)
scan-build:
    @rm -rf {{build_dir}}-scan
    scan-build --use-cc=clang --use-c++=clang++ -o scan-results \
        meson setup {{build_dir}}-scan -Dbuildtype=debug
    scan-build --use-cc=clang --use-c++=clang++ -o scan-results \
        meson compile -C {{build_dir}}-scan
    @echo "Scan results in scan-results/ directory"

# Run all static analyzers
analyze: lint cppcheck
    @echo "Static analysis complete"

# === Test Commands ===

# Run all tests (enables tests, builds, and runs)
test:
    @if [ ! -d {{build_dir}} ]; then meson setup {{build_dir}} -Denable_tests=true; fi
    @meson configure {{build_dir}} -Denable_tests=true >/dev/null
    meson compile -C {{build_dir}}
    meson test -C {{build_dir}}

# Run tests with verbose output
test-verbose:
    @if [ ! -d {{build_dir}} ]; then meson setup {{build_dir}} -Denable_tests=true; fi
    @meson configure {{build_dir}} -Denable_tests=true >/dev/null
    meson compile -C {{build_dir}}
    meson test -C {{build_dir}} -v

# Run tests matching a pattern (e.g., just test-filter "ZeroFill")
test-filter pattern:
    @if [ ! -d {{build_dir}} ]; then meson setup {{build_dir}} -Denable_tests=true; fi
    @meson configure {{build_dir}} -Denable_tests=true >/dev/null
    meson compile -C {{build_dir}}
    ./{{build_dir}}/storage_wiper_tests --gtest_filter="*{{pattern}}*"

# List all available tests
test-list:
    @if [ ! -d {{build_dir}} ]; then meson setup {{build_dir}} -Denable_tests=true; fi
    @meson configure {{build_dir}} -Denable_tests=true >/dev/null
    meson compile -C {{build_dir}}
    ./{{build_dir}}/storage_wiper_tests --gtest_list_tests

# === Memory Analysis Commands ===

# Run tests with valgrind to detect memory leaks
valgrind:
    @if [ ! -d {{build_dir}} ]; then meson setup {{build_dir}} -Denable_tests=true -Dbuildtype=debug; fi
    @meson configure {{build_dir}} -Denable_tests=true -Dbuildtype=debug >/dev/null
    meson compile -C {{build_dir}}
    valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
        --suppressions=.valgrind-suppressions \
        --error-exitcode=1 ./{{build_dir}}/storage_wiper_tests

# Run tests with valgrind (summary only)
valgrind-quick:
    @if [ ! -d {{build_dir}} ]; then meson setup {{build_dir}} -Denable_tests=true -Dbuildtype=debug; fi
    @meson configure {{build_dir}} -Denable_tests=true -Dbuildtype=debug >/dev/null
    meson compile -C {{build_dir}}
    valgrind --leak-check=summary --suppressions=.valgrind-suppressions \
        ./{{build_dir}}/storage_wiper_tests

# Run tests with valgrind and save report to file
valgrind-report:
    @if [ ! -d {{build_dir}} ]; then meson setup {{build_dir}} -Denable_tests=true -Dbuildtype=debug; fi
    @meson configure {{build_dir}} -Denable_tests=true -Dbuildtype=debug >/dev/null
    meson compile -C {{build_dir}}
    valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
        --suppressions=.valgrind-suppressions \
        --log-file=valgrind-report.txt ./{{build_dir}}/storage_wiper_tests
    @echo "Report saved to valgrind-report.txt"

# Run specific test with valgrind (e.g., just valgrind-filter "ZeroFill")
valgrind-filter pattern:
    @if [ ! -d {{build_dir}} ]; then meson setup {{build_dir}} -Denable_tests=true -Dbuildtype=debug; fi
    @meson configure {{build_dir}} -Denable_tests=true -Dbuildtype=debug >/dev/null
    meson compile -C {{build_dir}}
    valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
        --suppressions=.valgrind-suppressions \
        ./{{build_dir}}/storage_wiper_tests --gtest_filter="*{{pattern}}*"

# Generate valgrind suppression file from current run
valgrind-gen-suppressions:
    @if [ ! -d {{build_dir}} ]; then meson setup {{build_dir}} -Denable_tests=true -Dbuildtype=debug; fi
    @meson configure {{build_dir}} -Denable_tests=true -Dbuildtype=debug >/dev/null
    meson compile -C {{build_dir}}
    valgrind --leak-check=full --gen-suppressions=all \
        ./{{build_dir}}/storage_wiper_tests 2>&1 | grep -A 100 "^{" > valgrind-new-suppressions.txt || true
    @echo "New suppressions saved to valgrind-new-suppressions.txt"

# === Sanitizer Commands ===

# Build with AddressSanitizer (ASAN) - detects memory errors
build-asan:
    @if [ ! -d {{build_dir}} ]; then meson setup {{build_dir}}; fi
    meson configure {{build_dir}} -Dbuildtype=debug -Db_sanitize=address -Db_lundef=false
    meson compile -C {{build_dir}}

# Build with UndefinedBehaviorSanitizer (UBSAN) - detects undefined behavior
build-ubsan:
    @if [ ! -d {{build_dir}} ]; then meson setup {{build_dir}}; fi
    meson configure {{build_dir}} -Dbuildtype=debug -Db_sanitize=undefined
    meson compile -C {{build_dir}}

# Build with both ASAN and UBSAN
build-sanitizers:
    @if [ ! -d {{build_dir}} ]; then meson setup {{build_dir}}; fi
    meson configure {{build_dir}} -Dbuildtype=debug -Db_sanitize=address,undefined -Db_lundef=false
    meson compile -C {{build_dir}}

# Build with ThreadSanitizer (TSAN) - detects data races
build-tsan:
    @if [ ! -d {{build_dir}} ]; then meson setup {{build_dir}}; fi
    meson configure {{build_dir}} -Dbuildtype=debug -Db_sanitize=thread
    meson compile -C {{build_dir}}

# Run tests with ASAN
test-asan: build-asan
    @meson configure {{build_dir}} -Denable_tests=true >/dev/null
    meson compile -C {{build_dir}}
    ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 ./{{build_dir}}/storage_wiper_tests

# Run tests with UBSAN
test-ubsan: build-ubsan
    @meson configure {{build_dir}} -Denable_tests=true >/dev/null
    meson compile -C {{build_dir}}
    UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ./{{build_dir}}/storage_wiper_tests

# Run tests with all sanitizers
test-sanitizers: build-sanitizers
    @meson configure {{build_dir}} -Denable_tests=true >/dev/null
    meson compile -C {{build_dir}}
    ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
        ./{{build_dir}}/storage_wiper_tests

# Reset build to normal (no sanitizers)
build-reset:
    @if [ ! -d {{build_dir}} ]; then meson setup {{build_dir}}; fi
    meson configure {{build_dir}} -Dbuildtype=release -Db_sanitize=none -Db_lundef=true
    meson compile -C {{build_dir}}

# === Installation Commands ===

# Install to system (requires root)
install: build
    sudo meson install -C {{build_dir}}

# Uninstall from system
uninstall:
    sudo ninja -C {{build_dir}} uninstall 2>/dev/null || echo "Nothing to uninstall"

# === Packaging Commands ===

# Build Arch Linux package (release version from tarball)
pkg-arch:
    cd packaging/archlinux && makepkg -sf

# Install Arch Linux package (release)
pkg-arch-install: pkg-arch
    cd packaging/archlinux && sudo pacman -U storage-wiper-[0-9]*.pkg.tar.zst

# Build Arch Linux package (git version from local repo)
pkg-arch-git:
    cd packaging/archlinux && makepkg -sf -p PKGBUILD-git

# Install Arch Linux package (git version)
pkg-arch-git-install: pkg-arch-git
    cd packaging/archlinux && sudo pacman -U storage-wiper-git-*.pkg.tar.zst

# Clean Arch Linux package build artifacts
pkg-arch-clean:
    cd packaging/archlinux && rm -rf pkg src *.pkg.tar.zst *.log storage-wiper/

# === Info Commands ===

# Show build configuration
info:
    @echo "=== Meson Configuration ==="
    @meson configure {{build_dir}} 2>/dev/null || echo "Run 'just build' first"

# Show project structure
tree:
    @echo "=== Source Files ==="
    @find src -name '*.cpp' | sort
    @echo ""
    @echo "=== Header Files ==="
    @find src -name '*.hpp' | sort

# Count lines of code
loc:
    @echo "=== Lines of Code ==="
    @find src -name '*.cpp' -o -name '*.hpp' | xargs wc -l | tail -1

# === CLI Commands ===

# Build CLI tool
build-cli: build
    @echo "CLI tool built at {{build_dir}}/storage-wiper-cli"

# Run CLI - list disks
cli-list: build-cli
    ./{{build_dir}}/storage-wiper-cli --list

# Run CLI - list disks as JSON
cli-list-json: build-cli
    ./{{build_dir}}/storage-wiper-cli --list --json

# Run CLI with help
cli-help: build-cli
    ./{{build_dir}}/storage-wiper-cli --help

# === Log Commands ===

# View helper logs
logs-helper:
    sudo tail -f /var/log/storage-wiper/*.log 2>/dev/null || echo "No helper logs found (run 'sudo mkdir -p /var/log/storage-wiper' to create directory)"

# View GUI logs
logs-gui:
    tail -f ~/.local/share/storage-wiper/logs/*.log 2>/dev/null || echo "No GUI logs found"
