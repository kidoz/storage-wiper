# Algorithms

Algorithm IDs come from [WipeTypes.hpp](../../src/models/WipeTypes.hpp). Names,
pass counts, and execution behavior come from
[the algorithm implementations](../../src/algorithms/). The GUI and CLI use
the same helper implementations.

## Algorithm table

| ID | CLI name | Operation | Host overwrite passes | Readback verification | Application category |
| --- | --- | --- | --- | --- | --- |
| `0` | `zero-fill` | Write zeros | 1 | Full zero check | Clear |
| `1` | `random-fill` | Write OS-generated random data | 1 | Randomness check | Clear |
| `2` | `dod-5220-22-m` | DoD 5220.22-M pass sequence | 3 | Randomness check of final pass | Clear |
| `3` | `gutmann` | Gutmann pass sequence | 35 | Unsupported | Clear |
| `4` | `schneier` | Schneier pass sequence | 7 | Unsupported | Clear |
| `5` | `vsitr` | VSITR pass sequence | 7 | Unsupported | Clear |
| `6` | `gost` | GOST R 50739-95 pass sequence | 2 | Unsupported | Clear |
| `7` | `ata-secure-erase` | ATA/NVMe firmware erase | Device-managed | Unsupported | Purge |

The application's firmware-erase metadata reports `passes: 1`; this does not
mean one host overwrite pass. The category strings are `NIST 800-88 Clear` and
`NIST 800-88 Purge`, using the code's Rev. 1 mapping. See
[sanitization and current standards](../explanation/sanitization.md#category-labels-and-standards)
for the meaning and limits of these labels.

## Verification behavior

Zero Fill reads the device and compares against zero. Random Fill and DoD use
a chi-squared entropy check on the final random-data content; they do not
compare with a stored copy of the generated random stream.

Requesting verification for an unsupported algorithm results in
`verification_enabled: false`. It does not add a generic verification pass.
Verification completes before any post-success SSD discard.

## Firmware operation selection

For ATA drives, the implementation uses Security Erase and selects enhanced
erase when the drive advertises it. Drive security state can prevent execution.

For NVMe, the implementation selects a supported Sanitize operation in this
order: cryptographic erase, block erase, overwrite. When Sanitize is unavailable,
it can fall back to cryptographic Format NVM if the controller advertises the
required capability. Unsupported hardware fails the operation rather than
starting a software overwrite.

Hardware erase cannot target a partition. The helper treats NVMe hardware
erase as controller-wide for mount checks and exclusion of concurrent wipes;
see [device scope](../explanation/device_scope.md#nvme-namespaces).

For choosing a procedure, see [wipe a device](../how_to/wipe_device.md) and
[sanitization](../explanation/sanitization.md).

[Reference](README.md) · [Documentation home](../README.md)
