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
   namespaces in its subsystem, depending on the prepared method and scope.
4. Close applications using the target and unmount affected filesystems. Every device in the prepared erase scope must be unmounted and free of
   overlapping operations. Use another boot environment
   if the target contains your running system.
5. Review any LVM or device-mapper warning. An inactive LVM physical volume can
   still contain data belonging to logical volumes spread across multiple disks.
   Wiping the selected backing device can make those logical volumes unusable.
6. Choose an [algorithm](../reference/algorithms.md) and decide whether its
   supported verification meets your requirement. Review
   [sanitization limits](../explanation/sanitization.md) for SSDs or failing media.

Loop devices, device-mapper logical volumes, and arbitrary files are outside the
supported scope. See [device scope](../explanation/device_scope.md).

## Use the GUI

1. Select the intended device in the disk list.
2. Select the algorithm and enable verification if that algorithm supports it.
3. Request the wipe plan, authenticate if prompted, and read its device identity,
   firmware method, and scope in the confirmation.
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

To request cancellation, use the GUI cancel action for that job or press
Ctrl+C in its CLI process. Cancellation targets the prepared job ID, so it cannot
accidentally stop a newer job at a reused device path. Other jobs are unaffected.
Cancellation does not restore data already erased. Noncancellable firmware
operations remain monitored until they report a terminal result. See
[firmware cancellation](../explanation/sanitization.md#firmware-erasure).

## Close and reconnect

Closing the GUI while work is pending or running asks how to proceed. **Keep Open**
continues monitoring. **Stop and Close When Finished** discards pending
confirmations, requests cancellation of software wipes, and waits for terminal
results. Noncancellable firmware work must finish before that choice closes the
window. A cancelled software wipe leaves the target only partially erased.
**Leave Started Jobs Running and Close** abandons pending confirmations and
closes the GUI without cancelling accepted jobs. Reopen the GUI to monitor them
while the helper remains running.

When the GUI connects again, it can recover job snapshots from the running helper.
Recovered jobs retain their prepared device identity and original start time;
certificates do not substitute information from a replacement device at the same
path. A completion racing reconnection is recovered as a terminal result rather
than leaving the GUI waiting indefinitely.

The helper retains at most 64 jobs. Completed history expires after one hour and
may be evicted sooner when space is needed. Active jobs stay available while the
helper runs. History is not persistent across helper restart, power loss, or
forced termination. If a job disappears after such an event, do not interpret its
absence as a successful or cancelled erase; firmware may continue independently.

Stopping the helper normally requests cancellation of software work and waits
for noncancellable firmware monitoring to finish. This can delay service shutdown
for the duration of a firmware operation.

[How-to guides](README.md) · [Documentation home](../README.md)
