# Arch Linux Package Build Files

This directory contains PKGBUILD files for building Storage Wiper on Arch Linux.

## Available PKGBUILDs

| File | Description |
|------|-------------|
| `PKGBUILD` | Standard release package (for AUR submission) |
| `PKGBUILD-git` | Git version that tracks latest commits |
| `PKGBUILD-local` | Local development build from source directory |

## Building the Package

### From Release Tarball (Standard)

```bash
# Download the source tarball to this directory first
makepkg -si
```

### From Git Repository

```bash
cp PKGBUILD-git PKGBUILD
makepkg -si
```

### Local Development Build

```bash
cp PKGBUILD-local PKGBUILD
makepkg -si
```

## Updating .SRCINFO

After modifying the PKGBUILD, regenerate `.SRCINFO`:

```bash
makepkg --printsrcinfo > .SRCINFO
```

## Dependencies

**Runtime:**
- gtk4
- gtkmm-4.0
- libadwaita
- polkit (for privilege elevation)

**Build:**
- meson
- ninja

## Notes

- The package installs to `/usr` prefix (Arch Linux standard)
- Uses `arch-meson` wrapper for consistent build flags
- PolicyKit integration enables GUI privilege elevation via `pkexec`
