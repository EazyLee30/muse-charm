#!/usr/bin/env python3
"""Render the actual monochrome Charm UI on the host (C compiler + Pillow)."""
import ast, re, subprocess, tempfile
from pathlib import Path
from PIL import Image, ImageOps, ImageDraw
ROOT=Path(__file__).resolve().parents[1]

def main():
    tree=ast.parse((ROOT/'tests/test_rlcd42_avatar.py').read_text())
    prefix=next(ast.literal_eval(n.value) for n in ast.walk(tree) if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='prefix' for t in n.targets))
    old='static void charm_snapshot(charm_view_t *v) {*v=(charm_view_t){.wifi=true,.battery_pct=58,.clock="10:23"};}'
    prefix=re.sub(r'static void charm_snapshot[^\n]+','static charm_view_t view;\nstatic void charm_snapshot(charm_view_t *v) {*v=view;}',prefix)
    prefix=prefix.replace('#define RLCD_W 400','#define RLCD_W ((s_rotation&1)?300:400)').replace('#define RLCD_H 300','#define RLCD_H ((s_rotation&1)?400:300)').replace('#define RLCD_ROW_BYTES 50','#define RLCD_ROW_BYTES ((RLCD_W+7)/8)').replace('#define RLCD_FB_BYTES 15000','#define RLCD_FB_BYTES 15200').replace('static uint8_t s_fb[15000]','static uint8_t s_fb[15200]')
    source=(ROOT/'main/rlcd42_status.c').read_text();start=source.index('static inline void fb_px');end=source.index('// ---- Panel flush',start)
    suffix=r'''
int main(int argc,char **argv) {
 (void)argc;charm_font_init();
 view=(charm_view_t){.wifi=true,.battery_pct=98,.clock="10:23",.reaction=CHARM_PET};
 strcpy(s_caption,"你好！今天想听歌，还是陪我跳支舞？");s_caption_until=999999999;
 for(int frame=0;frame<24;frame++) for(int dark=0;dark<2;dark++) for(int rot=0;rot<4;rot++) {
   s_rotation=rot;view.key_pressed=frame%12<6;
   view.dark=dark;view.reaction_t=frame*.2f;fake_time=1000000+frame*200000;
   render_status("Muse",LED_STATE_WS_CONNECTED,LED_VOICE_SPEAKING,5);
   char name[512];snprintf(name,sizeof(name),"%s/%d-%d-%02d.pgm",argv[1],dark,rot,frame);
   FILE *f=fopen(name,"wb");fprintf(f,"P5\n%d %d\n255\n",RLCD_W,RLCD_H);
   for(int y=0;y<RLCD_H;y++) for(int x=0;x<RLCD_W;x++) {int ink=(s_fb[y*RLCD_ROW_BYTES+x/8]>>(7-x%8))&1;fputc((ink^dark)?0:255,f);}fclose(f);
 }
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        c=Path(tmp)/'preview.c';binary=Path(tmp)/'preview';c.write_text(prefix+source[start:end]+suffix)
        subprocess.run(['cc','-std=c11','-I',str(ROOT/'main'),'-I',str(ROOT/'components/muse'),str(c),str(ROOT/'avatar/muse_pixel.c'),str(ROOT/'main/pixel_font.c'),str(ROOT/'main/charm_font.c'),'-lm','-lz','-o',str(binary)],check=True)
        subprocess.run([str(binary),tmp],check=True)
        frames=[]
        for i in range(24):
            canvas=Image.new('RGB',(848,354),'#dce2df');d=ImageDraw.Draw(canvas)
            for dark,label in [(0,'PAPER / LIGHT'),(1,'INK / DARK')]:
                im=Image.open(Path(tmp)/f'{dark}-0-{i:02d}.pgm').convert('RGB');canvas.paste(im,(16+416*dark,38));d.text((16+416*dark,14),label,fill='#345347')
            frames.append(canvas)
        out=ROOT/'docs';out.mkdir(exist_ok=True)
        sheet=Image.new('RGB',(1464,906),'#dce2df');draw=ImageDraw.Draw(sheet)
        for dark in range(2):
            x=16
            for rot in range(4):
                im=Image.open(Path(tmp)/f'{dark}-{rot}-00.pgm').convert('RGB');sheet.paste(im,(x,36+450*dark));draw.text((x,14+450*dark),f"{rot*90} degrees / {'DARK' if dark else 'LIGHT'}",fill='#345347');x+=im.width+16
        sheet.save(out/'charm-orientations.png')
        frames[0].save(out/'charm-ui.png')
        frames[0].save(out/'charm-ui.gif',save_all=True,append_images=frames[1:],duration=200,loop=0)
if __name__=='__main__':main()
