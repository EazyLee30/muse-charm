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

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LED_C = ROOT / "main" / "led_status.c"
LED_H = ROOT / "main" / "led_status.h"


def _case_block(source: str, case: str) -> str:
    start = source.index(f"case {case}:")
    next_case = source.find("case LED_STATE_", start + 1)
    default = source.find("default:", start + 1)
    ends = [i for i in [next_case, default] if i != -1]
    end = min(ends) if ends else source.index("    }\n}", start)
    return source[start:end]


class LinkLedStatusContractTest(unittest.TestCase):
    def test_pairing_confirmation_pulses_then_setup_stays_solid_blue(self) -> None:
        header = LED_H.read_text()
        source = LED_C.read_text()

        self.assertIn("LED_STATE_SETUP_IDLE", header)
        self.assertIn("LED_STATE_PAIRING_CONFIRM_REQUIRED", header)
        confirm = _case_block(source, "LED_STATE_PAIRING_CONFIRM_REQUIRED")
        wifi = _case_block(source, "LED_STATE_WIFI_CONNECTED")

        self.assertIn("breathe(COLOR_BLUE, 2000)", confirm)
        self.assertIn(
            "case LED_STATE_WIFI_CONNECTING:\n            case LED_STATE_WIFI_CONNECTED:",
            source,
        )
        self.assertIn("led_hw_set_color(COLOR_BLUE)", wifi)
        self.assertNotIn("blink(", wifi)

    def test_wifi_auth_vm_progress_stays_blue_and_online_stays_green(self) -> None:
        source = LED_C.read_text()
        wifi_connected = _case_block(source, "LED_STATE_WIFI_CONNECTED")
        vm_ok = _case_block(source, "LED_STATE_VM_OK")
        online = _case_block(source, "LED_STATE_WS_CONNECTED")

        self.assertIn("led_hw_set_color(COLOR_BLUE)", wifi_connected)
        self.assertIn("case LED_STATE_AUTH_OK:\n            case LED_STATE_VM_OK:", source)
        self.assertIn("led_hw_set_color(COLOR_BLUE)", vm_ok)
        # Online is green: the whole LED, or a green dot on the display.
        self.assertIn("led_hw_set_connected()", online)
        self.assertIn("static void led_hw_set_connected(void) {\n"
                      "    led_hw_set_color(COLOR_GREEN);", source)
        dot = source[source.index("static uint16_t dot_px("):]
        self.assertIn("COLOR_GREEN", dot[:dot.index("\n}\n")])


if __name__ == "__main__":
    unittest.main()
