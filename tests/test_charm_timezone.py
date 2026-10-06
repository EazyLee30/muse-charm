"""Validate zone conversion, offset signs and daylight-saving rules."""
import subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class CharmTimezoneTest(unittest.TestCase):
 def test_offsets_dst_and_invalid_names(self):
  source=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include "charm_timezone.h"
static int hour(const char *zone,time_t timestamp) {
 char rule[80];assert(charm_timezone_resolve(zone,rule,sizeof(rule)));
 setenv("TZ",rule,1);tzset();return localtime(&timestamp)->tm_hour;
}
int main(void) {
 time_t winter=1767225600,summer=1782864000;char rule[80];
 assert(hour("UTC",winter)==0);
 assert(hour("Asia/Shanghai",winter)==8);
 assert(hour("UTC+05:30",winter)==5);
 assert(hour("UTC-07:00",winter)==17);
 assert(hour("America/Los_Angeles",winter)==16);
 assert(hour("America/Los_Angeles",summer)==17);
 assert(hour("Europe/London",winter)==0);
 assert(hour("Europe/London",summer)==1);
 assert(!charm_timezone_resolve("UTC+14:30",rule,sizeof(rule)));
 assert(!charm_timezone_resolve("UTC+08:99",rule,sizeof(rule)));
 assert(!charm_timezone_resolve("../invalid",rule,sizeof(rule)));
 assert(!charm_timezone_resolve("UTC+8:00",rule,sizeof(rule)));
 assert(!charm_timezone_resolve("Europe/Unknown",rule,sizeof(rule)));
}
'''
  with tempfile.TemporaryDirectory() as tmp:
   c=Path(tmp)/'test.c';binary=Path(tmp)/'test';c.write_text(source)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),str(c),str(ROOT/'main/charm_timezone.c'),'-o',str(binary)],check=True)
   subprocess.run([str(binary)],check=True)
