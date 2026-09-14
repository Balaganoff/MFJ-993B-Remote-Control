"""Host regression tests, also run before the PlatformIO firmware build."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

try:
    Import("env")
    root = Path(env["PROJECT_DIR"])
except NameError:
    root = Path(__file__).resolve().parents[1]

source_path = root / "firmware/MFJ993B_Remote_Control/MFJ993B_Remote_Control.ino"
source = source_path.read_text(encoding="utf-8")

# Do not build an accidental port with a changed electrical pin map or sampler.
assert "14, 26, 27, 33, 13, 2, 5, 21, 32" in source
assert "const uint32_t SAMPLE_DELAY = 110;" in source
assert "const uint32_t sampledBus = REG_READ(GPIO_IN_REG);" in source
assert "DISPLAY_IDLE_US" not in source and "pendingRawSignature" not in source
assert "resetCapturedLcd(true);" in source and "S000000000" in source

compiler, node = shutil.which("g++"), shutil.which("node")
if not compiler or not node:
    if os.environ.get("GITHUB_ACTIONS") == "true":
        raise RuntimeError("CI requires both g++ and node for regression tests")
    print("Host tests skipped: install g++ and node to run them locally.")
else:
    with tempfile.TemporaryDirectory(prefix="mfj-lcd-tests-") as directory:
        temporary = Path(directory)
        core = source[source.index("class LcdModel {"):source.index("CaptureRing captureRing;")]
        (temporary / "lcd_core_under_test.h").write_text(
            "#include <stdint.h>\n#include <string.h>\n" + core, encoding="utf-8")
        executable = temporary / "lcd-tests"
        subprocess.run([
            compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
            "-pthread", "-fsanitize=address,undefined",
            "-I", str(temporary), str(root / "tests/lcd_model_test.cpp"),
            "-o", str(executable)
        ], check=True)
        subprocess.run([str(executable)], check=True)
    subprocess.run([node, str(root / "tests/browser_test.js"), str(source_path)], check=True)
