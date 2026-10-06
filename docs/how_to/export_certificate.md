# Export a wipe certificate

Use this guide to keep the JSON and text record of a successful wipe. A
certificate records the tool's observed result; it is not digitally signed or
an independent assessment of the media.

## Find GUI certificates

The GUI automatically writes a pair after each successful wipe to
`$XDG_DATA_HOME/storage-wiper/certificates`, or
`~/.local/share/storage-wiper/certificates` when `XDG_DATA_HOME` is unset.

```bash
ls "${XDG_DATA_HOME:-$HOME/.local/share}/storage-wiper/certificates"
```

Each pair shares an automatically generated basename beginning with `wipe-`
and ending in the device name. The extensions are `.json` and `.txt`.

## Request a CLI certificate

Follow [wipe a device](wipe_device.md) to check the target first. Add
`--certificate` to the wipe command before starting. This **destructive
template** writes certificates under a directory in your home:

```bash
storage-wiper-cli --wipe /dev/DEVICE --algorithm zero-fill --verify \
  --certificate "$HOME/wipe-certificates/"
```

Replace `/dev/DEVICE` with your checked target. An existing directory, or a
path ending in `/`, receives an automatically named pair. A path without a
trailing slash that is not an existing directory is a basename: `report` writes
`report.json` and `report.txt`. Do not add `.json` to a basename unless you want
an additional `.json` extension.

Keep the certificate destination on storage outside the wipe scope. Reusing a
basename replaces its files. The helper does not retain an exportable history;
the CLI has no command to reconstruct a certificate for an earlier wipe.

## Validate and retain the record

1. Check that both files exist and can be read. A certificate write error can
   leave one file without its partner.
2. Check `device.path`, model/serial, size, algorithm, and timestamps against
   your intended operation.
3. Inspect `success`, `wipe.bad_block_count`, and the verification fields.
   `verification.enabled: false` means no supported readback verification ran.
4. Copy the pair to your audit storage and retain any device-specific evidence
   your sanitization procedure requires.

The CLI warns on certificate write failure but still returns success when the
wipe succeeded. Scripts that require a certificate must check the files as
well as the process exit status.

See the [certificate format](../reference/configuration.md#certificates) and
[verification explanation](../explanation/sanitization.md#what-verification-checks)
for interpreting the record.

[How-to guides](README.md) · [Documentation home](../README.md)
