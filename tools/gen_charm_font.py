#!/usr/bin/env python3
"""Rasterize an OFL Noto Sans CJK Medium subset, solid 1-bit pixels, no dithering.
Download the official OTF to .cache/fonts/NotoSansCJKsc-Medium.otf first.
"""
import re,zlib
from pathlib import Path
from PIL import Image, ImageFont, ImageDraw
root=Path(__file__).resolve().parents[1]
font=ImageFont.truetype(str(root/'.cache/fonts/NotoSansCJKsc-Medium.otf'),18)
s=(root/'components/muse/fonts/muse_font_cjk_16.c').read_text()
ranges=[tuple(map(int,m)) for m in re.findall(r'\.range_start = (\d+), \.range_length = (\d+), \.glyph_id_start = (\d+)',s)]
glyphs=list(range(32,127))
for start,length,_ in ranges:
 for cp in range(start,start+length):
  try:chr(cp).encode('gb2312')
  except UnicodeEncodeError:continue
  glyphs.append(cp)
glyphs=sorted(set(glyphs));bits=bytearray()
for cp in glyphs:
 im=Image.new('L',(18,22));ImageDraw.Draw(im).text((0,18),chr(cp),font=font,fill=255,anchor='ls')
 cell=bytearray(66)
 for y in range(22):
  for x in range(18):
   if im.getpixel((x,y))>=112:cell[y*3+x//8]|=128>>(x%8)
 bits.extend(cell)
out="// Generated from Noto Sans CJK SC Medium v2.004, Copyright 2014-2021 Adobe (http://www.adobe.com/).\n// SIL Open Font License 1.1; see docs/OFL-NotoSansCJK.txt.\n#include \"charm_font.h\"\nstatic const uint16_t codes[]={\n"
out+=','.join(str(v) for v in glyphs)+'};\nstatic const uint8_t advances[]={'+','.join(str(max(4,round(font.getlength(chr(cp))))) for cp in range(32,127))+'};\nstatic const uint8_t compressed[]={\n'
packed=zlib.compress(bits,9)
for i in range(0,len(packed),32):out+=','.join('0x%02x'%v for v in packed[i:i+32])+',\n'
out+='};\n'
out+='#include <stdlib.h>\n#ifdef ESP_PLATFORM\n#include "esp_heap_caps.h"\n#include "miniz.h"\n#else\n#include <zlib.h>\n#endif\nstatic uint8_t *bitmap;\nbool charm_font_init(void) {\n    if(bitmap) return true;\n    size_t size=sizeof(codes)/sizeof(codes[0])*CHARM_FONT_BYTES;\n#ifdef ESP_PLATFORM\n    uint8_t *data=heap_caps_malloc(size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);\n    tinfl_decompressor *state=heap_caps_malloc(sizeof(*state),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);\n    if(!data || !state) {free(data);free(state);return false;}\n    tinfl_init(state);size_t input=sizeof(compressed),output=size;\n    int result=tinfl_decompress(state,compressed,&input,data,data,&output,TINFL_FLAG_PARSE_ZLIB_HEADER|TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);\n    free(state);\n    if(result!=TINFL_STATUS_DONE || output!=size) {free(data);return false;}\n#else\n    uint8_t *data=malloc(size);uLongf output=size;\n    if(!data || uncompress(data,&output,compressed,sizeof(compressed))!=Z_OK || output!=size) {free(data);return false;}\n#endif\n    bitmap=data;return true;\n}\nconst uint8_t *charm_font_glyph(uint32_t cp) {\n    unsigned lo=0,hi=sizeof(codes)/sizeof(codes[0]);\n    while(lo<hi) {unsigned mid=(lo+hi)/2;if(codes[mid]<cp) lo=mid+1;else hi=mid;}\n    return bitmap && lo<sizeof(codes)/sizeof(codes[0]) && codes[lo]==cp ? bitmap+lo*CHARM_FONT_BYTES : 0;\n}\n'
out+='\nint charm_font_advance(uint32_t cp) {return cp>=32 && cp<127?advances[cp-32]:CHARM_FONT_W;}\n'
(root/'main/charm_font.c').write_text(out)
print('glyphs',len(glyphs),'flash bytes',len(packed)+len(glyphs)*2)
