#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""components/camera: the registry's one-holder rule for frames and streams and
backend start-up (camera_harness.c, against a scripted backend), and the SSCMA
transport's packets and reply framing (sscma_proto_harness.c)."""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CAMERA = ROOT / "components" / "camera"

# The bits of ESP-IDF the component uses.
ESP_ERR = """#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107
"""


class CameraTest(unittest.TestCase):
    def run_harness(self) -> bytes:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            self.skipTest("C compiler not available")
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            (out / "esp_err.h").write_text(ESP_ERR)
            binary = out / "camera_harness"
            proc = subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(out),
                                   "-I", str(CAMERA), str(ROOT / "tests" / "camera_harness.c"),
                                   str(CAMERA / "camera.c"), "-o", str(binary)],
                                  text=True, capture_output=True)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            proc = subprocess.run([str(binary)], capture_output=True)
            self.assertEqual(proc.returncode, 0, proc.stderr.decode())
            return proc.stdout

    def test_sscma_packets_and_replies(self) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            self.skipTest("C compiler not available")
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "sscma_proto_harness"
            proc = subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                                   "-I", str(CAMERA), str(ROOT / "tests" / "sscma_proto_harness.c"),
                                   str(CAMERA / "sscma_proto.c"), "-o", str(binary)],
                                  text=True, capture_output=True)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            proc = subprocess.run([str(binary)], capture_output=True)
            self.assertEqual((proc.returncode, proc.stdout), (0, b"ok\n"), proc.stderr.decode())

    def test_registry_frames_and_streams(self) -> None:
        self.assertEqual(self.run_harness(), b"ok\n")


if __name__ == "__main__":
    unittest.main()
