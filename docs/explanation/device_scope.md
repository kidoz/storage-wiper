# Device scope

Selecting a path is only the first step in understanding what will be erased.
Storage Wiper distinguishes a partition's address range, a whole disk, and
firmware operations whose scope can extend across an NVMe controller.

## Eligible devices

The helper discovers disks from `/sys/block` and includes their partitions.
Its whitelist covers canonical block-device paths in these families:

| Family | Typical whole disk | Typical partition |
| --- | --- | --- |
| SATA/SCSI/USB block storage | `/dev/sda` | `/dev/sda1` |
| NVMe namespace | `/dev/nvme0n1` | `/dev/nvme0n1p1` |
| SD/eMMC block storage | `/dev/mmcblk0` | `/dev/mmcblk0p1` |
| Virtio block storage | `/dev/vda` | `/dev/vda1` |

These are illustrative names, not recommended wipe targets. The helper resolves
paths and requires a block device; a string prefix alone is insufficient.
Loop, RAM, device-mapper, and other excluded virtual device families are not
offered as wipe targets. Arbitrary regular files are not eligible either.

Virtio's `/dev/vd*` family is explicitly allowed, so the device list can include
guest virtual disks. Eligibility does not mean the device is unused or that
its backing storage is suitable for a given sanitization requirement.

Sources: [DiskService](../../src/helper/services/DiskService.cpp) and
[device matching helpers](../../src/helper/services/DevicePathMatcher.hpp).

## Whole disks and partitions

A software overwrite of a whole disk covers its addressable range, including
the partition table and every partition. A software overwrite of a partition
covers that partition's range while leaving the partition table and sibling
partitions intact.

Partitions appear after their parent disk. They inherit the parent's model,
serial, and SMART information, but keep their own size and mount state. Shared
health information therefore does not imply shared wipe scope.

The helper rejects overlapping operations, such as a whole-disk wipe while a
partition on that disk is being wiped. Independent targets can run in parallel.
Hardware secure erase cannot be limited to a partition and is rejected for
partition targets.

## Mount state and exclusive claims

The clients display reported mount state; the helper checks eligibility again
when starting an operation. Whole-disk mount detection accounts for its
partitions. A partition's mount state does not adopt a sibling's mount point.
The wipe service also holds exclusive device claims for its operation lifetime.

These checks address known conflicts. They do not decide whether you intended
to select the device, whether a backup is adequate, or whether an external
storage stack is still relying on it. Device identities and mount state can
change between discovery and use, which is why target selection and the
helper's fresh checks both matter.

## LVM and device-mapper

Physical disks that contain LVM physical volumes can appear in the list. Their
device-mapper logical volumes, such as `/dev/dm-0` or `/dev/mapper/vg-lv`, are
excluded.

Erasing an underlying physical volume can destroy LVM metadata and data used
by the volume group, potentially affecting logical volumes spanning multiple
disks. Seeing a physical disk in the application is not evidence that its LVM
consumers have been deactivated. Prepare unused storage through your system's
storage-management workflow before wiping it; do not use the application's
unmount option as a substitute for understanding that dependency chain.

## NVMe namespaces

An NVMe controller can expose multiple namespaces, each shown as a block device.
A software overwrite operates on the selected namespace or partition. Firmware
operations can have a broader scope.

Storage Wiper treats an NVMe hardware erase conservatively as controller-wide:
the confirmation names the wider scope, every namespace must be unmounted, and
overlapping wipes on the controller are rejected. Device claims span the
hardware operation. A free selected namespace alone is insufficient if a
sibling is mounted or busy.

The CLI's `--force-unmount` applies to the selected mounted target; it does not
automatically prepare every sibling namespace for controller erase. Read the
scope statement and prepare the entire controller before confirming.

Sources: [WipeService](../../src/helper/services/WipeService.cpp),
[NVMe helpers](../../src/algorithms/NvmeSanitize.cpp), and
[ATA/NVMe firmware algorithm](../../src/algorithms/ATASecureEraseAlgorithm.cpp).

## Limits of the visible scope

The project does not currently implement HPA/DCO hidden-area detection. Host
overwrites reach addressable blocks, not every remapped or controller-managed
physical location. Bad sectors that cannot be written can retain data.

Device scope therefore needs to be considered alongside
[sanitization and verification](sanitization.md), rather than inferred solely
from the displayed size or successful completion. To perform an operation,
follow [wipe a device](../how_to/wipe_device.md).

[Explanation](README.md) · [Documentation home](../README.md)
