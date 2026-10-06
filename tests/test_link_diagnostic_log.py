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

from __future__ import annotations

import os
import re
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class LinkDiagnosticLogHarnessTest(unittest.TestCase):
    def test_normal_log_capture_harness(self) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            self.skipTest("C compiler not available")
        with tempfile.TemporaryDirectory() as tmpdir:
            binary = Path(tmpdir) / "link_diagnostic_log_harness"
            command = [
                *cc,
                "-std=c11",
                "-D_GNU_SOURCE",
                "-DLINK_FAKE_CUSTOM_HEAP_CAPS=1",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-g",
                "-O1",
                "-I",
                str(ROOT / "tests" / "link_fakes"),
                "-I",
                str(ROOT / "main"),
                str(ROOT / "tests" / "link_diagnostic_log_harness.c"),
                str(ROOT / "main" / "diagnostic_log.c"),
                "-o",
                str(binary),
            ]
            compiled = subprocess.run(
                command,
                cwd=ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self.assertEqual(
                compiled.returncode, 0, msg=compiled.stdout + compiled.stderr
            )
            ran = subprocess.run(
                [str(binary)],
                cwd=ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self.assertEqual(ran.returncode, 0, msg=ran.stdout + ran.stderr)

    def test_api_captures_normal_logs_without_typed_events(self) -> None:
        header = (ROOT / "main" / "diagnostic_log.h").read_text()
        source = (ROOT / "main" / "diagnostic_log.c").read_text()
        self.assertIn("esp_log_set_vprintf(diagnostic_log_vprintf)", source)
        self.assertIn("xSemaphoreTake(s_lock, 0)", source)
        self.assertIn('strncmp(tag, "link.", 5)', source)
        self.assertIn("_Atomic(vprintf_like_t)", source)
        self.assertIn("atomic_flag_test_and_set_explicit", source)
        self.assertNotIn("diagnostic_event_t", header + source)
        self.assertNotIn("diagnostic_log_record", header + source)

    def test_capture_format_is_pinned_and_heartbeat_is_separate(self) -> None:
        defaults = (ROOT / "sdkconfig.defaults").read_text()
        app = (ROOT / "main" / "app.c").read_text()
        self.assertIn("CONFIG_LOG_VERSION_1=y", defaults)
        self.assertIn("CONFIG_LOG_MODE_TEXT=y", defaults)
        self.assertIn("CONFIG_LOG_COLORS=n", defaults)
        self.assertIn("CONFIG_LOG_TIMESTAMP_SOURCE_RTOS=y", defaults)
        self.assertIn('HEARTBEAT_TAG = "link.heartbeat"', app)
        self.assertIn("ESP_LOGI(HEARTBEAT_TAG", app)

    def test_all_link_log_tags_are_namespaced(self) -> None:
        tag_pattern = re.compile(
            r"static const char \*(?:TAG|HEARTBEAT_TAG) = \"([^\"]+)\";"
        )
        macro_pattern = re.compile(r'#define\s+TAG\s+"([^"]+)"')
        literal_pattern = re.compile(r"ESP_LOG[A-Z]\(\"([^\"]+)\"")
        tags = []
        for path in (ROOT / "main").glob("*"):
            if path.suffix in {".c", ".cpp"}:
                source = path.read_text()
                tags.extend(tag_pattern.findall(source))
                tags.extend(macro_pattern.findall(source))
                tags.extend(literal_pattern.findall(source))
        self.assertTrue(tags)
        self.assertTrue(
            all(tag.startswith("link.") for tag in tags),
            msg=f"unnamespaced log tags: {tags}",
        )

    def test_secret_bearing_http_content_is_not_logged(self) -> None:
        ota = (ROOT / "main" / "ota.c").read_text()
        control = (ROOT / "main" / "noise_control.cpp").read_text()
        for forbidden in (
            "response header: %s: %s",
            "response body prefix",
            "final_url=%s",
            'request: %s %s',
            "format_url_for_log",
        ):
            self.assertNotIn(forbidden, ota)
        self.assertNotIn("upgrade rejected: '%.*s'", control)
        self.assertIn("upgrade rejected: status %.3s", control)
        # The overflow path reads the same response buffer, so it is held to the
        # same rule. Banning bounded-buffer printing outright is a stronger
        # guard than pinning one more message: any %.*s in this file would be
        # printing bytes off the wire.
        self.assertIn("upgrade response too large: status %.3s", control)
        self.assertNotIn("%.*s", control)
        kconfig = (ROOT / "main" / "Kconfig.projbuild").read_text()
        self.assertNotIn("full OTA\n            signed URLs", kconfig)
        self.assertIn("signed URL query parameters", kconfig)
        self.assertIn("arbitrary response bodies", kconfig)

    def test_support_relevant_failure_paths_use_normal_logs(self) -> None:
        ota = (ROOT / "main" / "ota.c").read_text()
        wifi = (ROOT / "main" / "wifi_mgr.c").read_text()
        for message in (
            "could not read image descriptor: %s",
            "incomplete image received",
            "could not start OTA: %s",
            "could not start OTA task",
        ):
            self.assertIn(message, ota)
        self.assertIn("disconnected reason=%u", wifi)


if __name__ == "__main__":
    unittest.main()
