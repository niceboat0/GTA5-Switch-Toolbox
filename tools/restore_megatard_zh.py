"""Restore selectable Chinese in the exact bundled MEGATARD v1.0.5 payload.

Keeps the six existing language IDs, every function address, native table and
gameplay function unchanged. Chinese is appended as language ID 6. Refuses any
other input build. This is a reproducible bytecode patch, not a source rebuild.
"""
import argparse,hashlib,struct
from pathlib import Path

ORIGINAL_SHA256='3bf7fbe1ec86e855de3bd1af168953bb2474a416ae43b4d22cba5bb2a1ac9658'
TR=0x1653;ROW=0xd62f;LANGUAGE_STATIC=1135

def pages(payload,field,length):
    table=struct.unpack_from('<Q',payload,field)[0]&0xffffff
    offsets=[struct.unpack_from('<Q',payload,table+i*8)[0]&0xffffff for i in range((length+0x3fff)//0x4000)]
    data=b''.join(payload[at:at+min(0x4000,length-i*0x4000)] for i,at in enumerate(offsets))
    if len(data)!=length:raise ValueError('Truncated payload pages')
    return table,offsets,data

class Assembler:
    def __init__(self):self.code=bytearray();self.labels={};self.fixups=[]
    def emit(self,*values):self.code.extend(values)
    def raw(self,value):self.code.extend(value)
    def label(self,name):self.labels[name]=len(self.code)
    def jump(self,name):
        self.emit(85,0,0);self.fixups.append((len(self.code)-2,len(self.code),name))
    def switch(self,cases):
        self.emit(101,len(cases))
        for value,name in cases:
            self.raw(struct.pack('<Ih',value,0));self.fixups.append((len(self.code)-2,len(self.code),name))
    def string(self,offset):self.emit(100,offset&255,(offset>>8)&255,(offset>>16)&255,102)
    def finish(self):
        for at,base,label in self.fixups:struct.pack_into('<h',self.code,at,self.labels[label]-base)
        return bytes(self.code)

def restore(original):
    if hashlib.sha256(original).hexdigest()!=ORIGINAL_SHA256:raise ValueError('Expected the exact unmodified MEGATARD v1.0.5 payload')
    payload=bytearray(original);code_length=struct.unpack_from('<I',payload,0x1c)[0]
    _,code_pages,raw_code=pages(payload,0x10,code_length);code=bytearray(raw_code)
    string_length=struct.unpack_from('<I',payload,0x70)[0]
    string_table,string_pages,strings=pages(payload,0x68,string_length)
    added=bytearray();zh=[]
    for text in ['英语','西班牙语','德语','俄语','葡萄牙语','法语','Chinese / 简体中文']:
        zh.append(string_length+len(added));added.extend(text.encode('utf-8')+b'\0')
    if string_length%0x4000+len(added)>0x4000:raise ValueError('Text additions exceed the existing last string page')

    # __TR takes eight original language arguments. The original six-language
    # dispatcher overwrites argument 2, then returns English for unsupported IDs.
    # Rebuild within its original 86 bytes, retaining all old ID -> argument maps.
    a=Assembler();a.raw(code[TR:TR+10]);a.emit(80,LANGUAGE_STATIC&255,LANGUAGE_STATIC>>8)
    mapping=[(1,1),(2,3),(3,5),(4,6),(5,7),(6,2)]
    a.switch([(language,'arg'+str(arg)) for language,arg in mapping])
    a.emit(56,0,46,8,1)
    for _,arg in mapping:a.label('arg'+str(arg));a.emit(56,arg,46,8,1)
    replacement=a.finish()
    if len(replacement)!=86:raise ValueError('Unexpected translation dispatcher size')
    code[TR:TR+86]=replacement

    # Factor repeated calls into one shared translation tail to fit seven rows
    # in the original 385-byte function; no code relocation is required.
    legacy=[
        [258770,258778,0,258796,0,258815,258846,258864],
        [258882,258890,0,258899,0,258919,258949,258969],
        [258989,258996,0,259014,0,259022,259049,259067],
        [259086,259094,0,259116,0,259142,259157,259180],
        [259203,259214,0,259225,0,259252,259214,259292],
        [259315,259322,0,259343,0,259368,259403,259424],
    ]
    a=Assembler();a.raw(code[ROW:ROW+25]);a.raw(raw_code[0xd648:0xd64f]);a.emit(56,0)
    a.switch([(i,'row'+str(i)) for i in range(7)])
    for index,offsets in enumerate(legacy):
        a.label('row'+str(index))
        for lang,offset in enumerate(offsets):
            if lang==2:a.string(zh[index])
            elif offset:a.string(offset)
            else:a.emit(113,102) # original empty/unavailable Japanese argument
        a.jump('translate')
    a.label('row6');a.string(zh[6]);a.emit(*([42]*7))
    a.label('translate');a.emit(93,TR&255,(TR>>8)&255,(TR>>16)&255,57,5)
    a.raw(raw_code[0xd783:0xd7b0])
    replacement=a.finish()
    if len(replacement)!=381:raise ValueError('Unexpected language row size')
    code[ROW:ROW+385]=replacement+bytes(4)

    # Seven choices, accepted on A and preserved in the existing 4-bit slot.
    changes=[
        (0xd5ff,b'\x77',b'\x78'),      # draw loop: index < 7
        (0x24736,b'\x77',b'\x78'),     # item count: 7
        (0x2da59,b'\x77',b'\x78'),     # apply selection: index < 7
        (0x2f015,b'\x76',b'\x77'),     # persistence maximum ID: 6
        (0x2f019,b'\x76',b'\x77'),     # persistence clamp: 6
        (0x4066b,b'\x0d',b'\x0e'),     # encoded ID maximum: 8 + 6
        (0x40776,b'\x76',b'\x77'),     # load valid IDs through 6
    ]
    for at,before,after in changes:
        if code[at:at+len(before)]!=before:raise ValueError('Unexpected language guard at '+hex(at))
        code[at:at+len(before)]=after
    for i,at in enumerate(code_pages):
        part=code[i*0x4000:(i+1)*0x4000];payload[at:at+len(part)]=part

    # Redirect only the final string page, leaving every existing string offset
    # and all unrelated resource data intact.
    last=strings[(len(string_pages)-1)*0x4000:]+added
    payload.extend(bytes((-len(payload))%16));new_offset=len(payload);payload.extend(last)
    struct.pack_into('<Q',payload,string_table+(len(string_pages)-1)*8,0x50000000|new_offset)
    struct.pack_into('<I',payload,0x70,string_length+len(added))
    return bytes(payload)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('input',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    try:result=restore(a.input.read_bytes())
    except (ValueError,OSError,struct.error) as e:p.exit(2,str(e)+'\n')
    a.output.write_bytes(result);print(hashlib.sha256(result).hexdigest())
