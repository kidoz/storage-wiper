# Wipe a device

Use this guide to erase a selected whole disk or partition through the installed
GUI or CLI. **The operation permanently destroys data in its scope.** Back up
anything you need before continuing.

## Identify the target and scope

1. List devices with `storage-wiper-cli --list` or open `storage_wiper` as your
   normal user.
2. Match the device path, model, size, and physical identity to the intended
   target. Use the GUI or an independent device inventory to check the serial
   when needed; the CLI JSON listing does not include it.
3. Decide whether you intend to erase a whole disk or one partition. Whole-disk
   overwrites erase the partition table and all partitions. Hardware erase
   requires a whole disk, and an NVMe hardware operation can affect all
   namespaces on its controller.
4. Close applications using the target and unmount affected filesystems. For
   NVMe hardware erase, every namespace in the controller scope must be
   unmounted and free of overlapping operations. Use another boot environment
   if the target contains your running system.
5. Choose an [algorithm](../reference/algorithms.md) and decide whether its
   supported verification meets your requirement. Review
   [sanitization limits](../explanation/sanitization.md) for SSDs or failing media.

Loop devices, device-mapper logical volumes, and arbitrary files are outside the
supported scope. See [device scope](../explanation/device_scope.md).

## Use the GUI

1. Select the intended device in the disk list.
2. Select the algorithm and enable verification if that algorithm supports it.
3. Start the wipe and read the confirmation's device identity and scope.
4. Confirm only if that scope matches your intent, and complete the polkit
   authentication prompt.
5. Monitor progress through writing and, when enabled, verification. Select
   another device to inspect its own progress during concurrent wipes.
6. Check the final result, verification verdict, and unwritable-sector count.
   A successful GUI wipe writes a certificate pair to the
   [configured data directory](../reference/configuration.md#certificates).

## Use the CLI

The following is a **destructive command template**. Replace `/dev/DEVICE`
with the exact eligible path you checked above; it is not a literal device name.

```bash
storage-wiper-cli --wipe /dev/DEVICE --algorithm zero-fill --verify
```

Read the displayed identity and scope, then type the exact word `yes` to
confirm. Polkit can request administrator authentication separately.

If the selected target is mounted and you want the helper to attempt its
unmount, add `--force-unmount`. This does not automatically unmount every
sibling namespace involved in an NVMe controller erase.

For a supported whole-disk firmware operation, use this destructive template
after checking the wider hardware scope:

```bash
storage-wiper-cli --wipe /dev/DEVICE --algorithm ata-secure-erase
```

The name also selects NVMe hardware erase. Unsupported firmware operations
fail; the application does not silently switch to a software overwrite.

`--yes` skips the CLI's typed confirmation for a prepared script. It does not
bypass polkit authorization, device validation, mount checks, or overlap checks.
Use it only after the script has established the target identity and scope.
See the [CLI reference](../reference/cli.md) for flags and exit statuses, and
[export a certificate](export_certificate.md) to record the result.

## Check completion or request cancellation

Wait for a terminal result. A write pass reaching 100% is not the same as
finishing verification. Treat a nonzero unwritable-sector count as evidence
that some locations may retain data, even when the operation reports success.

To request cancellation, use the GUI cancel action for that device or press
Ctrl+C in its CLI process. Other devices' wipes are unaffected. Cancellation
does not restore data already erased; a firmware command already accepted by
the device may continue running. See
[firmware cancellation](../explanation/sanitization.md#firmware-erasure).

[How-to guides](README.md) · [Documentation home](../README.md)
