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

`makepkg` keeps its work tree in `packaging/archlinux/src/`, including a Meson
build directory. After a version bump, add `-C` (`makepkg -sfC`) so that work
tree is removed first. A reused build directory stays configured for the previous
version's source, and the package then installs that older tree's files even
though the new archive's checksum was verified.

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

The [install hook](../../packaging/archlinux/storage-wiper.install) reloads systemd
unit definitions before D-Bus configuration. It preserves every running helper,
including apparently idle ones: a descriptor scan can race with authorization or
a newly started wipe. Replacing the executable does not update a live process.

After all operations reach a terminal result, restart the helper:

```bash
sudo systemctl restart storage-wiper-helper.service
```

For a helper directly activated by an older package, stop that specific process
only after all operations finish. The next client call activates the installed
build through the sandboxed systemd unit. An incompatible client reports a
protocol error until the helper is restarted. Do not remove the package during
a wipe: removal stops the helper. Shutdown waits for uncancellable firmware work.

Follow [inspect devices](../tutorials/inspect_devices.md) to check the installed
clients without starting an erase.

[How-to guides](README.md) · [Documentation home](../README.md)
