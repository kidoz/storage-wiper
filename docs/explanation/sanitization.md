# Sanitization and verification

A wipe result depends on what the operation can reach, how the device implements
it, and what evidence you require. Algorithm selection, verification, and a
certificate answer different parts of that problem.

## Host overwrites

Software algorithms write the selected block device's addressable range. Zero
Fill and Random Fill use one pass; the historical multipass algorithms repeat
writes with their own patterns. Pass counts are listed in the
[algorithm reference](../reference/algorithms.md).

Repeating host writes increases I/O and elapsed time. It does not make hidden,
retired, or remapped storage addressable. On flash devices, wear leveling and
controller-managed spare areas mean the host-visible range need not expose
every physical location that has held data. More overwrite passes therefore
do not by themselves establish sanitization of those areas.

For non-rotational devices, the helper can issue discard after a successful
software wipe. Verification happens first so it inspects the written content.
Discard is a device operation with its own implementation behavior; its use is
not independent evidence that every physical cell was erased.

## Firmware erasure

Hardware erase asks the device firmware to perform an operation the host cannot
implement through ordinary writes. Storage Wiper supports ATA Security Erase
and NVMe Sanitize, with cryptographic Format NVM as an NVMe fallback when its
capability checks permit it.

Firmware capabilities, security state, controller scope, and implementation
quality matter. The application reports the command result and available
progress; it does not implement a full independent assessment of a controller's
internal sanitization. There is no automatic fallback to a software overwrite
when hardware erase is unsupported.

Cancellation is also different from stopping a software write loop. Once a
firmware command has been accepted, stopping polling or requesting cancellation
may not stop the device operation. Erased data is not restored in either case.
See [device scope](device_scope.md) before selecting hardware erase, especially
on controllers with multiple NVMe namespaces.

## What verification checks

Storage Wiper implements readback verification for three algorithms:

- **Zero Fill:** compare the read content against zero.
- **Random Fill and DoD:** check the final random-data content with a
  chi-squared entropy test. This evaluates distribution rather than comparing
  every byte against a saved random stream.

The remaining algorithms have no supported readback verification in this
implementation. Requesting it does not create a generic additional check;
progress reports verification as disabled.

A readback verdict applies to the locations the verification can read. It does
not inspect hidden or remapped physical locations, establish firmware quality,
or prove an organizational sanitization procedure was followed. Write progress
reaching 100% is also separate from a completed verification verdict.

## Failing media

The write helper retries failures and attempts the remaining range in 512-byte
sectors, skipping sectors that remain unwritable. This lets an operation erase
as much accessible data as possible instead of abandoning the entire device on
the first failing sector.

The completion record carries the final pass's unwritable-sector count. A wipe
can report success with a nonzero count, and those sectors may retain data.
The count is not a cumulative sum of retries or passes and is not a map of all
physical locations that contain old data. A procedure requiring complete
sanitization must account for this evidence rather than interpreting success
alone as complete coverage.

## Category labels and standards

[The code's category mapping](../../src/models/WipeTypes.hpp) explicitly derives
from NIST SP 800-88 Rev. 1: software overwrites are labeled Clear and firmware
erase is labeled Purge. Those labels appear in the UI, CLI, and certificates.

[NIST SP 800-88 Rev. 2](https://csrc.nist.gov/pubs/sp/800/88/r2/final) was published
in September 2025 and superseded Rev. 1. Its guidance addresses a sanitization
program and selecting techniques appropriate to media and information
sensitivity. The application's existing labels are metadata, not a declaration
that a particular device operation has been independently validated against
the current guidance.

Likewise, an algorithm named for a historical DoD or national overwrite scheme
does not on its own establish compliance with a current policy. Evaluate the
medium, implementation, verification evidence, and applicable procedure.

## What a certificate records

Certificates preserve device identity, the selected algorithm and category,
timing, observed throughput, final bad-block count, and verification status.
They provide a useful audit record of what the application observed.

They are local, unsigned JSON/text files. They do not independently authenticate
the hardware result or certify standards compliance. CLI certificate-writing
errors do not change a successful wipe's exit status, so a workflow that requires
a record must check the files. See
[export a certificate](../how_to/export_certificate.md) and the
[certificate reference](../reference/configuration.md#certificates).

[Explanation](README.md) · [Documentation home](../README.md)
