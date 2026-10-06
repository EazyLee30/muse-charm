"""Exercise the actual setup-page JS against a split-line mock USB device."""
import re,shutil,subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class SetupPageTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which('node'),'Node.js required')
    def test_usb_workflow_and_failure_recovery(self):
        html=(ROOT/'tools/setup/index.html').read_text()
        script=re.search(r'<script>(.*?)</script>',html,re.S).group(1)
        harness=r'''
const vm=require('node:vm'),assert=require('node:assert/strict');
const elements={};
const document={getElementById(id){return elements[id]??={value:'',textContent:'',dataset:{},disabled:false};}};
let commands=[],waiter=null,chunks=[],failTTS=false,sdkConfigured=false,ttsConfigured=false,closed=0;
function push(text){const bytes=new TextEncoder().encode(text);for(const part of [bytes.slice(0,7),bytes.slice(7)]){if(waiter){const w=waiter;waiter=null;w({value:part,done:false});}else chunks.push(part);}}
const reader={read(){return chunks.length?Promise.resolve({value:chunks.shift(),done:false}):new Promise(r=>waiter=r);},cancel(){if(waiter){waiter({done:true});waiter=null;}return Promise.resolve();},releaseLock(){}};
const port={async open(){},async setSignals(){},async close(){closed++;},readable:{getReader:()=>reader},writable:{getWriter(){return {releaseLock(){},async write(bytes){const command=new TextDecoder().decode(bytes).trim();commands.push(command);
 if(command==='>setup.status')push('@setup '+JSON.stringify({ok:true,protocol:2,tts_provider:"qwen",board:'waveshare-s3-rlcd42',sdk_configured:sdkConfigured,tts_configured:ttsConfigured})+'\n');
 else if(command.startsWith('>sdk.setup=')){sdkConfigured=true;push('@setup {"ok":true,"restart_required":true}\n');}
 else if(command.startsWith('>tts.setup=')){ttsConfigured=!failTTS;push('@tts '+JSON.stringify({configured:!failTTS})+'\n');}
 else if(command==='>setup.restart')push('@setup {"ok":true,"restarting":true}\n');
 }}}}};
const context={document,navigator:{serial:{requestPort:async()=>port}},window:{isSecureContext:true},TextEncoder,TextDecoder,setTimeout,clearTimeout,console};
vm.createContext(context);
'''
        tests=r'''
(async()=>{
 assert(elements.save.disabled);
 elements.provider.value='qwen';await elements.connect.onclick();assert(!elements.save.disabled);
 elements.sdk.value='bad';await elements.credentials.onsubmit({preventDefault(){}});
 assert.match(elements.message.textContent,/格式/);assert.equal(commands.length,1);
 elements.sdk.value='mgst_synthetic_test_only';elements.tts.value='sk-synthetic-test-only';
 failTTS=true;await elements.credentials.onsubmit({preventDefault(){}});
 assert.match(elements.message.textContent,/SDK token 已保存/);assert.equal(elements.sdk.value,'');assert.equal(elements.tts.value,'');
 assert(!commands.includes('>setup.restart'));assert(!elements.restart.disabled);
 await elements.restart.onclick();assert(closed>0);assert(elements.save.disabled);
 failTTS=false;await elements.connect.onclick();
 elements.provider.value='minimax-global';elements.provider.onchange();
 assert(!elements.minimaxOptions.hidden);
 elements.model.value='speech-2.8-turbo';elements.voice.value='male-qn-qingse';
 assert.match(elements.sdkState.textContent,/已配置/);
 const before=commands.filter(x=>x.startsWith('>sdk.setup=')).length;
 elements.tts.value='sk-synthetic-test-only';await elements.credentials.onsubmit({preventDefault(){}});
 assert.equal(commands.filter(x=>x.startsWith('>sdk.setup=')).length,before);
 assert(commands.some(x=>x.includes('minimax-global')));assert(commands.includes('>setup.restart'));assert(elements.save.disabled);
 assert.equal(elements.tts.value,'');assert(!Object.values(elements).some(e=>e.textContent.includes('synthetic')));
 console.log('USB page workflow passed');
})().catch(e=>{console.error(e);process.exitCode=1;});
'''
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)/'page.cjs';p.write_text(harness+'\nvm.runInContext('+repr(script)+',context);\n'+tests)
            subprocess.run(['node',str(p)],check=True,timeout=20)
