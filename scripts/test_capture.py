"""Exercise the actual menu start/stop actions, microphone, and a slow device startup."""

import json
import os
from pathlib import Path
import subprocess
import tempfile


root = Path(__file__).resolve().parent.parent
app = root / "bin/Ascolto.app/Contents/MacOS/Ascolto"

with tempfile.TemporaryDirectory(prefix="ascolto-capture-") as directory:
    for name, delay, inventory_delay, rounds in (
        ("real-microphone", None, None, 3),
        ("slow-device", "5000", None, 1),
        ("slow-device-inventory", None, "5000", 1),
    ):
        config = Path(directory) / f"{name}.json"
        config.write_text(json.dumps({"auto_trigger_mic": False, "save_audio": False,
                                      "output_dir": directory, "language": "it"}))
        environment = os.environ.copy()
        environment.pop("ASCOLTO_CAPTURE_TEST_START_DELAY_MS", None)
        environment.pop("ASCOLTO_DEVICE_TEST_DELAY_MS", None)
        environment["ASCOLTO_CAPTURE_TEST_ROUNDS"] = str(rounds)
        if delay:
            environment["ASCOLTO_CAPTURE_TEST_START_DELAY_MS"] = delay
        if inventory_delay:
            environment["ASCOLTO_DEVICE_TEST_DELAY_MS"] = inventory_delay
        with (Path(directory) / f"{name}.log").open("w+") as log:
            process = subprocess.Popen([str(app), "--config", str(config), "--capture-smoke-test"],
                                       cwd=root, env=environment, stdout=log, stderr=subprocess.STDOUT)
            try:
                result = process.wait(timeout=rounds * 7 + 11)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                result = 124
            log.seek(0)
            output = log.read()
        print(f"[{name}] exit={result}")
        print(output, end="", flush=True)
        if (result or output.count("Start: PASS") != rounds or output.count("Stop: PASS") != rounds
                or "FAIL" in output):
            raise SystemExit(result or 1)
        if inventory_delay:
            for expected in ("Menu during slow inventory: PASS",
                             "Start completion during menu tracking: PASS",
                             "Device cache populated: PASS"):
                if expected not in output:
                    raise SystemExit(f"Missing verification: {expected}")
