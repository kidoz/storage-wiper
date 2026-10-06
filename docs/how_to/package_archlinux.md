# Package for Arch Linux

Use this guide on Arch Linux to build the release or Git package. Build as a
normal user with the usual Arch packaging tools and the dependencies declared
in the selected PKGBUILD. Use `sudo` only for package installation or service
management.

## Build the release package

From the repository root:

```bash
cd packaging/archlinux
makepkg -sf
```

The [release PKGBUILD](../../packaging/archlinux/PKGBUILD) downloads the tagged
source archive and verifies its SHA-256 checksum. It does not package uncommitted
changes from your working tree. `-s` installs missing dependencies through
pacman; `-f` permits replacing an existing package artifact.

Install the artifacts named by the current recipe rather than globbing every
old package in the directory:

```bash
makepkg --packagelist | xargs -r sudo pacman -U
```

The repository equivalents are `just pkg-arch` and `just pkg-arch-install` from
the repository root.

## Build the Git package

From `packaging/archlinux`:

```bash
makepkg -p PKGBUILD-git -sf
makepkg -p PKGBUILD-git --packagelist | xargs -r sudo pacman -U
```

The [Git PKGBUILD](../../packaging/archlinux/PKGBUILD-git) fetches upstream Git
source and derives its version from the fetched `meson.build`, commit count,
and short hash. Its VCS source uses `SKIP`; the fixed release archive has an
explicit checksum. The Git package provides and conflicts with the release
package.

The repository equivalents are `just pkg-arch-git` and
`just pkg-arch-git-install`.

## Prepare release metadata

When maintaining the release PKGBUILD:

1. Set `pkgver` to the published tag's version and update `pkgrel` as appropriate.
2. Verify the intended archive and record its SHA-256 digest in `sha256sums`.
   Keep an explicit digest for the fixed release source.
3. Build the package and review its installed files and dependency metadata.
4. Regenerate `.SRCINFO` from the release recipe:

   ```bash
   makepkg --printsrcinfo > .SRCINFO
   ```

5. Include matching metadata when publishing through your packaging workflow.
   A separate Git-package submission needs metadata generated with
   `makepkg -p PKGBUILD-git --printsrcinfo`.

For the packaging tool's options, consult the installed `makepkg(8)` manual.

## Activate an upgraded helper

The [install hook](../../packaging/archlinux/storage-wiper.install) reloads
D-Bus configuration and systemd unit definitions on upgrade. It leaves the
running helper in place so an upgrade does not interrupt a wipe.

After every wipe has reached a terminal result, restart a systemd-managed
helper to load the new executable:

```bash
sudo systemctl restart storage-wiper-helper.service
```

This command manages the systemd unit. A helper launched directly by D-Bus
activation must also release its bus name before the new systemd instance can
start; see [helper activation](../explanation/architecture.md#helper-activation).
Do not remove the package during a wipe: its removal hook stops the helper.

Follow [inspect devices](../tutorials/inspect_devices.md) to check the installed
clients without starting an erase.

[How-to guides](README.md) · [Documentation home](../README.md)
