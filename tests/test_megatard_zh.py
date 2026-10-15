"""Execute the patched language bytecode, including selection/save/load guards.

This small VM only implements instructions reached by these language paths.
Game drawing/stat natives are captured fixtures; this is not a console test.
"""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('restore_megatard_zh',ROOT/'tools/restore_megatard_zh.py')
patch=importlib.util.module_from_spec(spec);spec.loader.exec_module(patch)

def function_entry(code,instructions,address):
    seen=set()
    while address not in seen:
        seen.add(address);assert address in instructions
        if code[address]==45:return address
        if code[address]==0:address+=1
        elif code[address]==85:address+=3+struct.unpack_from('<h',code,address+1)[0]
        else:raise AssertionError(('call entry',address,code[address]))
    raise AssertionError('cyclic function trampoline')

def decode(code):
    sizes={37:2,38:3,39:4,40:5,41:5,44:4,46:3,
           **{i:2 for i in list(range(52,63))+[64,65,66,104,105,106,107]},
           **{i:3 for i in range(67,93)},**{i:4 for i in range(93,101)}}
    instructions={};functions={};at=0
    while at<len(code):
        op=code[at];assert op<=129,('opcode',at,op)
        size=sizes.get(op,1)
        if op==45:
            size=5+code[at+4];functions[at]=code[at+5:at+size].rstrip(b'\0').decode()
        if op==101:size=2+code[at+1]*6
        assert at+size<=len(code)
        instructions[at]=(op,size);at+=size
    for at,(op,size) in instructions.items():
        if 85<=op<=92:
            assert at+3+struct.unpack_from('<h',code,at+1)[0] in instructions
        if op==101:
            for i in range(code[at+1]):
                end=at+2+(i+1)*6;assert end+struct.unpack_from('<h',code,end-2)[0] in instructions
        if op==93:
            target=int.from_bytes(code[at+1:at+4],'little')
            assert function_entry(code,instructions,target) in functions
    return instructions,functions

class VM:
    def __init__(self,payload):
        self.code=patch.pages(payload,0x10,struct.unpack_from('<I',payload,0x1c)[0])[2]
        self.strings=patch.pages(payload,0x68,struct.unpack_from('<I',payload,0x70)[0])[2]
        self.ins,self.names=decode(self.code);self.statics={};self.hooks={};self.native=None
    def string(self,index):return self.strings[index:].split(b'\0',1)[0].decode('utf-8')
    def function(self,address):
        return function_entry(self.code,self.ins,address)
    def run(self,address,args=()):
        address=self.function(address)
        if address in self.hooks:return self.hooks[address](args)
        c=self.code;assert c[address]==45 and c[address+1]==len(args)
        frame={i:v for i,v in enumerate(args)};stack=[];pc=address;steps=0
        while steps<200000:
            steps+=1;op,size=self.ins[pc];next_pc=pc+size
            if op in [0,45]:pass
            elif op==37:stack.append(c[pc+1])
            elif op in [38,39]:stack.extend(c[pc+1:next_pc])
            elif op==40:stack.append(struct.unpack_from('<i',c,pc+1)[0])
            elif op==41:stack.append(struct.unpack_from('<f',c,pc+1)[0])
            elif 112<=op<=120:stack.append(op-113)
            elif 121<=op<=129:stack.append(float(op-122))
            elif op==42:stack.append(stack[-1])
            elif op==43:stack.pop()
            elif op in [55,56,57]:
                index=c[pc+1]
                if op==55:stack.append(('frame',frame,index))
                elif op==56:stack.append(frame.get(index,0))
                else:frame[index]=stack.pop()
            elif op in [59,60,80,81]:
                index=c[pc+1] if op<67 else struct.unpack_from('<H',c,pc+1)[0]
                if op in [59,80]:stack.append(self.statics.get(index,0))
                else:self.statics[index]=stack.pop()
            elif op==67:stack.append(struct.unpack_from('<h',c,pc+1)[0])
            elif op==100:stack.append(int.from_bytes(c[pc+1:pc+4],'little'))
            elif op==102:stack.append(self.string(stack.pop()))
            elif op in [61,68]:stack[-1]+=c[pc+1] if op==61 else struct.unpack_from('<h',c,pc+1)[0]
            elif op in [1,2,3,8,9,10,11,12,13,14,15,16,31,32]:
                b=stack.pop();a=stack.pop()
                operations={1:lambda:a+b,2:lambda:a-b,3:lambda:a*b,8:lambda:int(a==b),9:lambda:int(a!=b),
                    10:lambda:int(a>b),11:lambda:int(a>=b),12:lambda:int(a<b),13:lambda:int(a<=b),
                    14:lambda:a+b,15:lambda:a-b,16:lambda:a*b,31:lambda:a&b,32:lambda:a|b}
                stack.append(operations[op]())
            elif op==6:stack.append(int(not stack.pop()))
            elif 85<=op<=92:
                take=op==85
                if op==86:take=not stack.pop()
                elif op>=87:
                    b=stack.pop();a=stack.pop();take={87:a!=b,88:a==b,89:a<=b,90:a<b,91:a>=b,92:a>b}[op]
                if take:next_pc=pc+3+struct.unpack_from('<h',c,pc+1)[0]
            elif op==101:
                value=stack.pop()
                for i in range(c[pc+1]):
                    at=pc+2+i*6;case,delta=struct.unpack_from('<Ih',c,at)
                    if value==case:next_pc=at+6+delta;break
            elif op==93:
                target=self.function(int.from_bytes(c[pc+1:pc+4],'little'));count=c[target+1]
                call_args=tuple(stack[-count:]) if count else ()
                if count:del stack[-count:]
                stack.extend(self.run(target,call_args))
            elif op==44:
                count=c[pc+1]>>2;returns=c[pc+1]&3;index=(c[pc+2]<<8)|c[pc+3]
                call_args=tuple(stack[-count:]) if count else ()
                if count:del stack[-count:]
                assert self.native is not None,(hex(pc),index,call_args)
                result=self.native(address,index,call_args);assert len(result)==returns;stack.extend(result)
            elif op==46:
                returns=c[pc+2];assert c[pc+1]==len(args)
                assert len(stack)==returns,(hex(pc),'unbalanced stack',stack)
                return stack
            else:raise AssertionError(('unimplemented reached instruction',hex(pc),op))
            pc=next_pc
        raise AssertionError('VM instruction budget exceeded')

def original_from_restored(restored,fixture):
    original=bytearray(restored[:fixture['original_size']]);length=struct.unpack_from('<I',original,0x1c)[0]
    _,positions,raw=patch.pages(original,0x10,length);code=bytearray(raw)
    for at,hex_data in fixture['original_code_regions']:code[at:at+len(bytes.fromhex(hex_data))]=bytes.fromhex(hex_data)
    for i,at in enumerate(positions):part=code[i*0x4000:(i+1)*0x4000];original[at:at+len(part)]=part
    for at,hex_data in fixture['original_file_regions']:original[at:at+len(bytes.fromhex(hex_data))]=bytes.fromhex(hex_data)
    return bytes(original)

def test(output):
    output.mkdir(parents=True,exist_ok=True);cases=[]
    def check(name,condition):assert condition,name;cases.append(name)
    fixture=json.loads((ROOT/'tests/fixtures/megatard-1.0.5-restore.json').read_text())
    restored=(ROOT/'romfs/builtin_mods/megatard/ragemenu.nsc').read_bytes()
    original=original_from_restored(restored,fixture)
    check('only documented language regions and appended text differ',hashlib.sha256(original).hexdigest()==fixture['original_sha256'])
    check('patch is exactly reproducible',patch.restore(original)==restored and hashlib.sha256(restored).hexdigest()==fixture['restored_sha256'])
    try:patch.restore(restored);rejected=False
    except ValueError:rejected=True
    check('unknown/already-patched payload refused',rejected)
    old,new=VM(original),VM(restored)
    check('function addresses and debug names preserved',old.names==new.names)
    args=tuple(range(100,108))
    for language in range(6):
        old.statics[1135]=new.statics[1135]=language
        check('existing language '+str(language)+' unchanged',old.run(patch.TR,args)==new.run(patch.TR,args))
    new.statics[1135]=6;check('Chinese dispatch uses original Chinese argument',new.run(patch.TR,args)==[102])
    check('Chinese gameplay label resolves to translated text',new.run(patch.TR,tuple(['PLAYER SETTINGS','ES','玩家设置','DE','JA','RU','PT','FR']))==['玩家设置'])
    for language in range(7):
        old.statics[1135]=new.statics[1135]=language;old.statics[3]=new.statics[3]=2
        old_rows=[];new_rows=[];old.hooks[0x1ff24]=lambda a:old_rows.append(a) or [];new.hooks[0x1ff24]=lambda a:new_rows.append(a) or []
        for row in range(7):new.run(patch.ROW,(row,0.25))
        if language<6:
            for row in range(6):old.run(patch.ROW,(row,0.25))
            check('legacy language row labels '+str(language)+' unchanged',old_rows==new_rows[:6])
        else:check('all seven language choices readable in Chinese',[r[1] for r in new_rows]==['英语','西班牙语','德语','俄语','葡萄牙语','法语','Chinese / 简体中文'])
        check('Chinese entry visible in language '+str(language),new_rows[6][1]=='Chinese / 简体中文')
    new.statics={2:7,1136:1};check('language navigation exposes seven items',new.run(0x244e1)==[7])
    saved=[];new.hooks[0x31a73]=lambda a:saved.append(new.statics[1135]) or []
    def timer(function,index,a):
        assert function==0x2bbb2 and index==389 and not a
        return [1000]
    new.native=timer
    new.statics[3]=6;new.run(0x2bbb2);check('A selects Chinese and requests persistence',new.statics[1135]==6 and saved==[6])
    new.statics[3]=7;new.run(0x2bbb2);check('out-of-range language selection refused',saved==[6])
    new.hooks.pop(0x31a73);slots={}
    new.hooks[0x2e835]=lambda a:[a[0]] # same-build stat key fixture for each profile
    def native(function,index,a):
        if function==0x2efe7:slots[a[0]]=a[1];return [1]
        if function==0x40625:
            if a[0] not in slots:return [0]
            _,frame,key=a[1];frame[key]=slots[a[0]];return [1]
        raise AssertionError(('unexpected native',hex(function),index,a))
    new.native=native
    for language in range(7):
        new.run(0x2efe7,(0,language));check('language '+str(language)+' round-trips through stat slot',new.run(0x40625,(0,))==[language])
    slots[0]=15;check('invalid encoded language refused',new.run(0x40625,(0,))==[-1])
    new.hooks[0x31b69]=lambda a:[0];new.hooks[0x2ec80]=lambda a:[0]
    new.statics={1135:6};new.run(0x31a73);check('Chinese saved for all three profiles',slots=={0:14,1:14,2:14})
    new.hooks[0x40f73]=lambda a:[-1];new.statics[1135]=0;new.run(0x40719)
    check('fresh load restores persisted Chinese language',new.statics[1135]==6)
    report={'passed':len(cases),'cases':cases,'console_verified':False,'sha256':hashlib.sha256(restored).hexdigest()}
    (output/'megatard-zh-tests.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'megatard_zh_checks':len(cases)}))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);test(p.parse_args().output)
