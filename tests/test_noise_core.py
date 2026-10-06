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
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "components" / "noise_core"
EMBEDDED_CXX_FLAGS = ["-fno-exceptions", "-fno-rtti"]


def _cxx_command() -> list[str]:
    cmd = shlex.split(os.environ.get("CXX", "c++"))
    if not cmd or shutil.which(cmd[0]) is None:
        raise unittest.SkipTest("C++ compiler not available")
    return cmd


def _psa_crypto_flags(cxx: list[str], tmp: Path) -> list[str]:
    flags: list[str] | None = None
    if shutil.which("pkg-config") is not None:
        proc = subprocess.run(
            ["pkg-config", "--cflags", "--libs", "mbedcrypto"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if proc.returncode == 0:
            flags = shlex.split(proc.stdout)
    if flags is None:
        flags = ["-lmbedcrypto"]

    probe = tmp / "psa_crypto_probe"
    probe_proc = subprocess.run(
        [
            *cxx,
            "-x",
            "c++",
            "-std=c++17",
            "-o",
            str(probe),
            "-",
            *flags,
        ],
        input=(
            "#include <psa/crypto.h>\n"
            "int main() { return psa_crypto_init() == PSA_SUCCESS ? 0 : 1; }\n"
        ),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if probe_proc.returncode != 0:
        raise unittest.SkipTest(
            "PSA Crypto development headers/libraries not available: "
            + probe_proc.stderr.strip()
        )
    return flags


class NoiseCoreCompileTest(unittest.TestCase):
    def test_imported_core_compiles_and_links_with_psa_crypto(self) -> None:
        cxx = _cxx_command()
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            psa_crypto_flags = _psa_crypto_flags(cxx, tmp)
            binary = tmp / "noise_core_harness"
            srcs = [
                COMPONENT / "src" / "ClientSession.cpp",
                COMPONENT / "src" / "InitiatorHandshake.cpp",
                COMPONENT / "src" / "PsaCryptoBackend.cpp",
                COMPONENT / "src" / "ServiceCodec.cpp",
                COMPONENT / "src" / "Status.cpp",
                COMPONENT / "src" / "Transport.cpp",
                COMPONENT / "src" / "TransportFrameCodec.cpp",
            ]
            compile_cmd = [
                *cxx,
                "-std=c++17",
                "-Wall",
                "-Wextra",
                *EMBEDDED_CXX_FLAGS,
                "-g",
                "-O1",
                "-I",
                str(COMPONENT / "include"),
                str(ROOT / "tests" / "noise_core_harness.cpp"),
                *(str(src) for src in srcs),
                "-pthread",
                *psa_crypto_flags,
                "-o",
                str(binary),
            ]
            compile_proc = subprocess.run(
                compile_cmd,
                cwd=ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self.assertEqual(
                compile_proc.returncode,
                0,
                msg=compile_proc.stdout + compile_proc.stderr,
            )

            run_proc = subprocess.run(
                [str(binary)],
                cwd=ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self.assertEqual(
                run_proc.returncode,
                0,
                msg=run_proc.stdout + run_proc.stderr,
            )


if __name__ == "__main__":
    unittest.main()
