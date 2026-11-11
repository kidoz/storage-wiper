#!/usr/bin/env python3
"""Check service wiring and package hooks without touching the host manager."""

import configparser
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ServiceIntegrationTest(unittest.TestCase):
    def test_activation_selects_matching_sandboxed_unit(self):
        activation = configparser.ConfigParser(interpolation=None)
        activation.read(ROOT / "data/dbus/su.kidoz.storage_wiper.Helper.service.in")
        unit = configparser.ConfigParser(interpolation=None, strict=False)
        unit.read(ROOT / "data/dbus/storage-wiper-helper.service.in")
        self.assertEqual(activation["D-BUS Service"]["SystemdService"],
                         "storage-wiper-helper.service")
        self.assertEqual(activation["D-BUS Service"]["Name"], unit["Service"]["BusName"])
        self.assertEqual(activation["D-BUS Service"]["Exec"], unit["Service"]["ExecStart"])
        self.assertEqual(unit["Service"]["Type"], "dbus")
        self.assertEqual(unit["Service"]["ProtectSystem"], "strict")
        self.assertEqual(unit["Service"]["TimeoutStopSec"], "infinity")
        self.assertEqual(unit["Service"]["NoNewPrivileges"], "true")
        self.assertEqual(unit["Service"]["CapabilityBoundingSet"],
                         "CAP_SYS_ADMIN CAP_SYS_RAWIO CAP_DAC_OVERRIDE")
        unit_text = (ROOT / "data/dbus/storage-wiper-helper.service.in").read_text()
        self.assertNotIn("DeviceAllow=char-*", unit_text)
        self.assertNotIn("DeviceAllow=block-*", unit_text)

    def run_hook(self, hook, running):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            calls = directory / "calls"
            for command in ("systemctl", "pkill"):
                path = directory / command
                path.write_text('#!/bin/sh\nprintf "%s\\n" "' + command + ' $*" >> "$CALLS"\n')
                path.chmod(0o755)
            pgrep = directory / "pgrep"
            pgrep.write_text("#!/bin/sh\nexit " + ("0" if running else "1") + "\n")
            pgrep.chmod(0o755)
            environment = {"PATH": str(directory), "CALLS": str(calls)}
            result = subprocess.run(
                ["/bin/bash", "-c", '. "$1"; "$2" 2.0', "hook-test",
                 str(ROOT / "packaging/archlinux/storage-wiper.install"), hook],
                env=environment, capture_output=True, text=True, check=True)
            return calls.read_text().splitlines(), result.stdout

    def test_upgrade_preserves_running_helper(self):
        calls, output = self.run_hook("post_upgrade", True)
        self.assertEqual(calls, ["systemctl daemon-reload", "systemctl reload dbus.service"])
        self.assertIn("running helper was preserved", output)

    def test_install_reloads_manager_before_bus(self):
        calls, output = self.run_hook("post_install", False)
        self.assertEqual(calls, ["systemctl daemon-reload", "systemctl reload dbus.service"])
        self.assertNotIn("running helper was preserved", output)

    def test_removal_stops_unit_and_legacy_helper(self):
        calls, _ = self.run_hook("post_remove", True)
        self.assertEqual(calls[:2], ["systemctl stop storage-wiper-helper.service",
                                     "systemctl disable storage-wiper-helper.service"])
        self.assertTrue(calls[2].startswith("pkill "))
        self.assertEqual(calls[-2:], ["systemctl daemon-reload", "systemctl reload dbus.service"])


if __name__ == "__main__":
    unittest.main()
