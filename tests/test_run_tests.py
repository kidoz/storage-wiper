"""Regression checks for credential isolation at the Meson process boundary."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class TestRunnerEnvironmentTest(unittest.TestCase):
    def run_wrapper(self, fake_meson, arguments):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "meson"
            executable.write_text("#!/usr/bin/env python3\n" + fake_meson)
            executable.chmod(0o700)
            environment = dict(os.environ)
            environment["PATH"] = directory + os.pathsep + os.environ["PATH"]
            environment["ANTHROPIC_AUTH_TOKEN"] = "test-canary-not-a-real-secret"
            environment["UNRECOGNIZED_FUTURE_CREDENTIAL"] = "test-canary-not-a-real-secret"
            wrapper = Path(__file__).resolve().parents[1] / "scripts/run_tests.py"
            return subprocess.run([sys.executable, str(wrapper), *arguments], env=environment,
                                  capture_output=True, check=False).returncode

    def test_credentials_do_not_reach_meson_and_arguments_are_preserved(self):
        result = self.run_wrapper('''import os, sys
assert "ANTHROPIC_AUTH_TOKEN" not in os.environ
assert "UNRECOGNIZED_FUTURE_CREDENTIAL" not in os.environ
assert sys.argv[1:] == ["test", "-C", "build with spaces", "--no-rebuild"]
''', ["-C", "build with spaces", "--no-rebuild"])
        self.assertEqual(result, 0)

    def test_test_failure_is_not_hidden(self):
        self.assertEqual(self.run_wrapper("import sys\nsys.exit(7)\n", []), 7)


if __name__ == "__main__":
    unittest.main()
