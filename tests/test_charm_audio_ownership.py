"""Finishing a Muse chat only cancels audio owned by that chat."""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

class AudioOwnershipTest(unittest.TestCase):
    def test_session_completion_preserves_hardware_audio(self):
        source = (ROOT / 'components/muse/muse_tts.c').read_text()
        functions = source[source.index('void muse_tts_cancel(void)'):source.index('static bool begin(')]
        harness = '#include <stdatomic.h>\n#include <stdbool.h>\n#include <assert.h>\nstatic atomic_uint generation;\nstatic atomic_bool session_owned;\n' + functions + '''
int main(void) {
 atomic_store(&generation,7);
 atomic_store(&session_owned,false);
 muse_tts_cancel_session(); assert(atomic_load(&generation)==7);
 atomic_store(&session_owned,true);
 muse_tts_cancel_session(); assert(atomic_load(&generation)==8);
 atomic_store(&session_owned,false);
 muse_tts_cancel(); assert(atomic_load(&generation)==9);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            c = Path(tmp) / 'audio.c'; binary = Path(tmp) / 'audio'
            c.write_text(harness)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Werror', str(c), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
