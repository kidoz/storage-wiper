# Inspect devices

In this tutorial you will inspect storage devices through the CLI and GUI,
recognize partition entries, and read the available health information. You
will not start a wipe.

## Before you start

Follow [install](../how_to/install.md) first. Use a normal account in a desktop
session with the D-Bus system bus and a polkit authentication agent available.
The installed CLI and GUI must be on your `PATH`.

## 1. Check the client

```bash
storage-wiper-cli --version
```

The CLI prints its version. This confirms that your shell can find the client;
it does not yet test the helper connection.

## 2. List devices

```bash
storage-wiper-cli --list
```

The client connects to the root helper and displays eligible disks and their
partitions. The shipped policy allows an active local user to list devices
without an administrator prompt; other sessions can require authentication.

Look for a whole-disk path such as `/dev/sda` or `/dev/nvme0n1`. A partition has
a path such as `/dev/sda1` or `/dev/nvme0n1p1`. Your machine's names and list will
differ. An empty list is possible when no eligible devices are present.

## 3. Read the JSON form

```bash
storage-wiper-cli --list --json
```

The result is a JSON array. Locate `path`, `size_bytes`, `is_partition`, and
`parent_disk` on one entry. A whole disk has `is_partition: false` and an empty
`parent_disk`; a partition names its parent.

Locate `is_mounted` and `mount_point`. These describe the reported mount state,
not permission to erase a device. SMART values can be `null` when unavailable.
The [CLI reference](../reference/cli.md#json-device-list) lists all fields.

## 4. Open the GUI

```bash
storage_wiper
```

The application displays the disk list and algorithms. Select a device and
compare its size, mount state, and health with the CLI listing. Select a
partition, if one is present, and observe that its health information belongs
to the parent disk. Leave the wipe action untouched.

![Storage Wiper device selection](../images/main.png)

This screenshot illustrates the layout; your device list and labels may differ.
Close the window after inspecting the list.

You have now used both clients to read device information. Before erasing a
device, read [device scope](../explanation/device_scope.md) and follow
[wipe a device](../how_to/wipe_device.md). For connection errors or missing
devices, use [troubleshooting](../how_to/troubleshoot.md).

[Tutorials](README.md) · [Documentation home](../README.md)
