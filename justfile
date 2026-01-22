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
    find src tests -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i

# Check formatting without modifying files
format-check:
    @find src tests -name '*.cpp' -o -name '*.hpp' | xargs clang-format --dry-run -Werror 2>&1 \
        && echo "All files formatted correctly" \
        || (echo "Run 'just format' to fix formatting"; exit 1)

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
        --error-exitcode=1 ./{{build_dir}}/storage_wiper_tests

# Run tests with valgrind (summary only)
valgrind-quick:
    @if [ ! -d {{build_dir}} ]; then meson setup {{build_dir}} -Denable_tests=true -Dbuildtype=debug; fi
    @meson configure {{build_dir}} -Denable_tests=true -Dbuildtype=debug >/dev/null
    meson compile -C {{build_dir}}
    valgrind --leak-check=summary ./{{build_dir}}/storage_wiper_tests

# Run tests with valgrind and save report to file
valgrind-report:
    @if [ ! -d {{build_dir}} ]; then meson setup {{build_dir}} -Denable_tests=true -Dbuildtype=debug; fi
    @meson configure {{build_dir}} -Denable_tests=true -Dbuildtype=debug >/dev/null
    meson compile -C {{build_dir}}
    valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
        --log-file=valgrind-report.txt ./{{build_dir}}/storage_wiper_tests
    @echo "Report saved to valgrind-report.txt"

# Run specific test with valgrind (e.g., just valgrind-filter "ZeroFill")
valgrind-filter pattern:
    @if [ ! -d {{build_dir}} ]; then meson setup {{build_dir}} -Denable_tests=true -Dbuildtype=debug; fi
    @meson configure {{build_dir}} -Denable_tests=true -Dbuildtype=debug >/dev/null
    meson compile -C {{build_dir}}
    valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
        ./{{build_dir}}/storage_wiper_tests --gtest_filter="*{{pattern}}*"

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
