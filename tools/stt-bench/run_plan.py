#!/usr/bin/env python3
"""Execute a prepared native benchmark plan sequentially, retaining failures."""
import json
import os
from pathlib import Path
import subprocess
import sys

plan = json.loads(Path(sys.argv[1]).read_text())
for entry in plan:
    env = dict(os.environ, **entry.get("environment", {}))
    command = [sys.executable, str(Path(__file__).with_name("run_serial.py")),
               "--id", entry["id"], "--timeout", str(entry.get("timeout", 1800)),
               "--", *entry["command_argv"]]
    print(json.dumps(dict(starting=entry["id"])), flush=True)
    subprocess.run(command, env=env)
