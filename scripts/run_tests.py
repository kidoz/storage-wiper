#!/usr/bin/env python3
"""Run Meson tests without inheriting unrelated credentials into test logs."""

import os
import subprocess
import sys

# Meson records its complete environment, including in testlog.json. An allowlist
# avoids depending on the spelling of every present or future credential variable.
TEST_ENVIRONMENT = ("PATH", "HOME", "LANG", "LC_ALL", "TMPDIR")


def main():
    environment = {name: os.environ[name] for name in TEST_ENVIRONMENT if name in os.environ}
    return subprocess.call(["meson", "test", *sys.argv[1:]], env=environment)


if __name__ == "__main__":
    sys.exit(main())
