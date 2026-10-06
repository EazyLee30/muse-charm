"""Validate percentage-to-ES8311 gain mapping against its 0.5 dB scale."""
import subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class VolumeTest(unittest.TestCase):
    def test_gain_mapping(self):
        source=(ROOT/'main/voice_board_waveshare_s3_rlcd42.c').read_text()
        a=source.index('static uint8_t dac_volume(')
        b=source.index('\nvoid voice_board_set_volume',a)
        body=r'''
int main(void) {
 assert(dac_volume(-1)==0 && dac_volume(0)==0);
 assert(dac_volume(100)==191 && dac_volume(200)==191);
 assert(dac_volume(60)==182); /* -4.5 dB, close to 20 log10(.6) */
 assert(dac_volume(50)==179); /* -6 dB */
 assert(dac_volume(10)==151); /* -20 dB */
 for(int i=1;i<100;i++) {
   assert(dac_volume(i)<=dac_volume(i+1));
   assert(dac_volume(i)<=191);
   float db=(dac_volume(i)-191)*.5f;
   assert(fabsf(db-20*log10f(i/100.0f))<=.251f);
 }
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            c,binary=Path(tmp)/'volume.c',Path(tmp)/'volume'
            c.write_text('#include <stdint.h>\n#include <math.h>\n#include <assert.h>\n'+source[a:b]+body)
            subprocess.run(['cc','-std=c11',str(c),'-lm','-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
