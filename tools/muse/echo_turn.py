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

"""Reset a board, hold talk for 2.5 s over serial ('d'/'u'), print the log: tools/muse/echo_turn.py <port>

Look for "recorded 3.08s" and the Muse turn logs (with Muse not set up, the press just shows why).
"""
import sys
import time

import serial

s = serial.Serial(sys.argv[1], 115200, timeout=0.2)
s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False


def rd(t):
    end = time.time() + t
    out = b''
    while time.time() < end:
        out += s.read(4096)
    return out.decode('utf-8', 'replace')


o = rd(6)
s.write(b'd'); o += rd(2.5); s.write(b'u'); o += rd(10)
sys.stdout.write(o)
