#!/usr/bin/env python3
"""Local USB control. API credentials come from a private JSON file, never argv."""
import argparse,json,time
from pathlib import Path
import serial

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port',default='/dev/cu.usbmodem1101')
    sub=p.add_subparsers(dest='action',required=True)
    sub.add_parser('status')
    c=sub.add_parser('configure');c.add_argument('--mode',choices=['dark','light','toggle']);c.add_argument('--reaction',choices=['calm','pet','dance','wave','sleep','hop','peek','stretch']);c.add_argument('--location');c.add_argument('--timezone');c.add_argument('--rotate',choices=['left','right','reset'])
    c=sub.add_parser('speak');c.add_argument('text')
    sub.add_parser('pair');sub.add_parser('disconnect');sub.add_parser('sensors')
    c=sub.add_parser('list');c.add_argument('path',nargs='?',default='')
    c=sub.add_parser('read');c.add_argument('path')
    c=sub.add_parser('music');c.add_argument('source',nargs='?');c.add_argument('--stop',action='store_true')
    c=sub.add_parser('tts-setup');c.add_argument('private_json_file',type=Path)
    a=p.parse_args();params={};prefix='@charm '
    if a.action=='tts-setup':
        private=json.loads(a.private_json_file.read_text());key=private.get('key','')
        if not key or len(key)>=256: p.error('private JSON needs a key shorter than 256 bytes')
        command='tts.setup='+json.dumps({'key':key});prefix='@tts '
    elif a.action=='configure':
        params={k:getattr(a,k) for k in ['mode','reaction','location','rotate','timezone'] if getattr(a,k) is not None};command='charm.configure='+json.dumps(params,ensure_ascii=False)
    elif a.action=='speak':command='charm.speak='+json.dumps({'text':a.text},ensure_ascii=False)
    elif a.action in ['pair','disconnect']:command='charm.controller='+json.dumps({'action':a.action})
    elif a.action=='sensors':command='charm.sensors'
    elif a.action in ['list','read']:command='charm.storage.'+a.action+'='+json.dumps({'path':a.path},ensure_ascii=False)
    elif a.action=='music':
        if not a.stop and not a.source:p.error('music needs an HTTP(S) MP3 URL or SD path, or --stop')
        params={'action':'stop'} if a.stop else ({'path':a.source} if not a.source.startswith(('http://','https://')) else {'url':a.source})
        command='charm.music='+json.dumps(params,ensure_ascii=False)
    else:command='charm.status'
    if len(command.encode())>=1023:p.error('command exceeds USB line limit')
    with serial.Serial(a.port,115200,timeout=.2) as s:
        s.write(('>'+command+'\n').encode());deadline=time.monotonic()+15;buffer=b''
        while time.monotonic()<deadline:
            buffer+=s.read(4096)
            while b'\n' in buffer:
                line,buffer=buffer.split(b'\n',1);line=line.decode('utf8','replace').strip()
                if line.startswith(prefix):
                    try:result=json.loads(line[len(prefix):])
                    except ValueError:continue
                    print(line);return 0 if result.get('ok',result.get('configured',False)) else 1
    print('No command acknowledgement; check that no other program owns the serial port.');return 1
if __name__=='__main__':raise SystemExit(main())
