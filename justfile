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

# Run clang-tidy (if available)
lint:
    @if [ ! -d {{build_dir}} ]; then meson setup {{build_dir}} -Denable_clang_tidy=true; fi
    meson compile -C {{build_dir}} clang-tidy || echo "clang-tidy not available"

# Run cppcheck (if available)
cppcheck:
    @if [ ! -d {{build_dir}} ]; then meson setup {{build_dir}} -Denable_cppcheck=true; fi
    meson compile -C {{build_dir}} cppcheck || echo "cppcheck not available"

# Format code (requires clang-format)
format:
    find src -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i

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

# === Installation Commands ===

# Install to system (requires root)
install: build
    sudo meson install -C {{build_dir}}

# Uninstall from system
uninstall:
    sudo ninja -C {{build_dir}} uninstall 2>/dev/null || echo "Nothing to uninstall"

# === Packaging Commands ===

# Build Arch Linux package (local)
pkg-arch:
    cd packaging/archlinux && cp PKGBUILD-local PKGBUILD && makepkg -sf

# Install Arch Linux package
pkg-arch-install: pkg-arch
    cd packaging/archlinux && sudo pacman -U storage-wiper-*.pkg.tar.zst

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
