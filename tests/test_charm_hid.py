"""Parse an actual HID layout rather than assuming raw Xbox button offsets."""
import subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class HidTest(unittest.TestCase):
 def test_report_ids_padding_and_bounds(self):
  code=r'''
#include <assert.h>
#include "charm_hid.h"
int main(void) {
 const uint8_t descriptor[]={0x05,1,0x09,5,0xa1,1,0x85,3,
   0x05,9,0x19,1,0x29,8,0x15,0,0x25,1,0x75,1,0x95,8,0x81,2,
   0x05,1,0x09,0x39,0x15,1,0x25,8,0x75,4,0x95,1,0x81,0x42,
   0x75,4,0x95,1,0x81,1,0xc0};
 charm_hid_map_t map;assert(charm_hid_parse(&map,descriptor,sizeof(descriptor)));
 uint8_t report[]={5,3};uint16_t buttons;int hat;
 assert(charm_hid_input(&map,3,report,2,&buttons,&hat));assert(buttons==5 && hat==2);
 assert(!charm_hid_input(&map,3,report,1,&buttons,&hat));
 assert(!charm_hid_input(&map,4,report,2,&buttons,&hat));
 report[1]=0;assert(charm_hid_input(&map,3,report,2,&buttons,&hat));assert(hat==-1);
 const uint8_t truncated[]={0x75};assert(!charm_hid_parse(&map,truncated,1));
 const uint8_t overflow[]={0x75,32,0x96,255,255,0x81,2};assert(!charm_hid_parse(&map,overflow,7));
 const uint8_t pop[]={0xb4};assert(!charm_hid_parse(&map,pop,1));
}
'''
  with tempfile.TemporaryDirectory() as tmp:
   c=Path(tmp)/'test.c';b=Path(tmp)/'test';c.write_text(code)
   subprocess.run(['cc','-std=c11','-I',str(ROOT/'main'),str(c),str(ROOT/'main/charm_hid.c'),'-o',str(b)],check=True)
   subprocess.run([str(b)],check=True)
