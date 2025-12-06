# Storage Wiper Development Commands
# Usage: just <command>

# Default recipe - show available commands
default:
    @just --list

# === Build Commands ===

# Configure and build (release)
build:
    @if [ ! -d builddir ]; then meson setup builddir; fi
    meson compile -C builddir

# Configure and build (debug)
build-debug:
    @if [ ! -d builddir-debug ]; then meson setup builddir-debug -Dbuildtype=debug; fi
    meson compile -C builddir-debug

# Clean build directory and rebuild
rebuild:
    rm -rf builddir
    meson setup builddir
    meson compile -C builddir

# Clean all build artifacts
clean:
    rm -rf builddir builddir-debug

# === Run Commands ===

# Run application (requires root for disk access, preserves DBUS session)
run: build
    sudo -E DBUS_SESSION_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS" ./builddir/storage_wiper

# Run debug build
run-debug: build-debug
    sudo -E DBUS_SESSION_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS" ./builddir-debug/storage_wiper

# Run with GTK inspector enabled
run-inspect: build
    GTK_DEBUG=interactive sudo -E DBUS_SESSION_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS" ./builddir/storage_wiper

# Run without root (limited functionality - can view disks but not wipe)
run-noroot: build
    ./builddir/storage_wiper

# Run via pkexec (production-like, shows auth dialog)
run-pkexec: build
    pkexec ./builddir/storage_wiper

# === Development Commands ===

# Watch for changes and rebuild (requires entr)
watch:
    find src -name '*.cpp' -o -name '*.hpp' | entr -c just build

# Check for compiler warnings
check: build
    meson compile -C builddir 2>&1 | grep -E "warning:|error:" || echo "No warnings or errors"

# Run clang-tidy (if available)
lint:
    @if [ ! -d builddir ]; then meson setup builddir -Denable_clang_tidy=true; fi
    meson compile -C builddir clang-tidy || echo "clang-tidy not available"

# Run cppcheck (if available)
cppcheck:
    @if [ ! -d builddir ]; then meson setup builddir -Denable_cppcheck=true; fi
    meson compile -C builddir cppcheck || echo "cppcheck not available"

# Format code (requires clang-format)
format:
    find src -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i

# === Installation Commands ===

# Install to system (requires root)
install: build
    sudo meson install -C builddir

# Uninstall from system
uninstall:
    sudo ninja -C builddir uninstall 2>/dev/null || echo "Nothing to uninstall"

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
    @meson configure builddir 2>/dev/null || echo "Run 'just build' first"

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
