"""Prepare verified, paired imports for the NRO; no installed files are edited.

Self Radio consumes its minimal HARD release and builder verification record.
Radio imports consume completed Radio Editor station + timing outputs together.
"""
import argparse,hashlib,json,re,shutil,struct
from pathlib import Path

def sha(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

def create(output,id,title,zh,kind,entries):
    if not re.fullmatch(r'[a-z0-9][a-z0-9_-]{0,62}',id):raise ValueError('Use a short lowercase package id')
    for text in (title,zh):
        if not text or len(text.encode('utf-8'))>=96 or any(ord(c)<32 or c=='|' for c in text):
            raise ValueError('Package titles must be nonempty, single-line UTF-8 text shorter than 96 bytes')
    if output.exists():raise FileExistsError('Use a new export folder')
    output.mkdir(parents=True);(output/'payload').mkdir()
    lines=['GTA5-TOOLBOX-PACK|1','id|'+id,'title|'+title,'title_zh|'+zh,'kind|'+kind]
    for i,(mode,target,entry,source,before) in enumerate(entries):
        dest='payload/'+str(i)+'.bin';digest=sha(source);shutil.copy2(source,output/dest)
        if sha(output/dest)!=digest:raise ValueError('Source changed while preparing the package')
        lines.append('|'.join((mode,target,entry,dest,digest,before)))
    (output/'package.txt').write_text('\n'.join(lines)+'\n',encoding='utf-8')
    print('Copy',output,'to /switch/gta5save/packages/'+id)

def selfradio(args):
    folder=args.input;record=args.manifest or folder.parent.parent/'release-verification.json'
    metadata=json.loads(record.read_text());assets=metadata['archive_replacements']['replacements']
    files=[('RPF','romfs/update/update.rpf',r['archive_path'],folder/r['package_path'],r['baseline_payload_sha256']) for r in assets]
    if len(files)!=16:raise ValueError('Expected all sixteen Self Radio assets')
    for source,r in zip(files,assets):
        if sha(source[3])!=r['sha256']:raise ValueError('Changed Self Radio archive asset')
    exefs=folder/'atmosphere/contents/0100b00b51230000/exefs'
    module=exefs/'subsdk4';digest=metadata.get('confirmed_module_sha256')
    if not digest or sha(module)!=digest:raise ValueError('Expected the confirmed Self Radio module')
    # Known initial v1.1 module; an occupied foreign subsdk4 is refused.
    files.append(('FILE','exefs/subsdk4','',module,'480c9f1eee304d2f34b87363e169d8f91521ce481f403792873fdd87f26cf4f3'))
    npdm=exefs/'main.npdm';old=bytearray(npdm.read_bytes())
    aci,_,acid,_=struct.unpack_from('<4I',old,0x70)
    for base,field in [(aci,0x20),(acid,0x220)]:
        offset=base+struct.unpack_from('<I',old,base+field)[0]+4
        flags=struct.unpack_from('<Q',old,offset)[0];struct.pack_into('<Q',old,offset,flags & ~(1<<21))
    before=sha(args.npdm_before) if args.npdm_before else hashlib.sha256(old).hexdigest()
    files.append(('FILE','exefs/main.npdm','',npdm,before))
    create(args.output,'self-radio-nx-1_1','Self Radio NX v1.1','自定义电台 NX v1.1','selfradio',files)

def rpf_names(file):
    with file.open('rb') as f:
        head=f.read(16)
        if len(head)!=16:raise ValueError('Truncated RPF header')
        magic,count,names_len,enc=struct.unpack('<4I',head);shift=names_len>>28&7;names_len&=0x0fffffff
        if magic!=0x52504637 or enc!=0x4e45504f or not 0<count<=200000 or not 0<names_len<=8388608:
            raise ValueError('Only valid unencrypted OPEN RPF7 outputs are supported')
        toc=f.read(count*16);names=f.read(names_len);size=file.stat().st_size
    if len(toc)!=count*16 or len(names)!=names_len:raise ValueError('Truncated RPF directory')
    found=[]
    for i in range(count):
        a,b,raw,flags=struct.unpack_from('<4I',toc,i*16)
        directory=b==0x7fffff00
        offset=(a if directory else a&65535)<<shift;end=names.find(b'\0',offset)
        if not 0<=offset<len(names) or end<0:raise ValueError('Invalid RPF entry name')
        name=names[offset:end].decode('utf-8')
        if directory:
            if raw+flags>count:raise ValueError('RPF child range exceeds directory')
            continue
        q=a|(b<<32);start=((q>>40)&0x7fffff)*512;stored=(q>>16)&0xffffff
        if start<16+count*16+names_len or start>=size or (not q>>63 and start+(stored or raw)>size):
            raise ValueError('RPF payload exceeds archive')
        found.append(name)
    return found

def radio(args):
    if not args.station.name.startswith('RADIO_') or args.station.suffix.lower()!='.rpf':
        raise ValueError('Choose a rebuilt RADIO_*.rpf station output')
    if args.timing.name.lower()!='audio_rel.rpf':raise ValueError('Choose the paired rebuilt audio_rel.rpf timing output')
    if not any(name.lower().endswith('.awc') for name in rpf_names(args.station)):
        raise ValueError('Station archive has no AWC audio entries')
    if not any(name.lower().endswith('.dat54.rel') for name in rpf_names(args.timing)):
        raise ValueError('Timing archive has no dat54 sound metadata')
    files=[('FILE','romfs/switch/audio/sfx/'+args.station.name.lower(),'',args.station,sha(args.station_before) if args.station_before else '-'),
           ('FILE','romfs/switch/audio/audio_rel.rpf','',args.timing,sha(args.timing_before) if args.timing_before else '-')]
    create(args.output,args.id,args.title,args.title_zh or args.title,'radio',files)

if __name__=='__main__':
    p=argparse.ArgumentParser();subs=p.add_subparsers(dest='command',required=True)
    s=subs.add_parser('selfradio');s.add_argument('--input',type=Path,required=True);s.add_argument('--manifest',type=Path);s.add_argument('--npdm-before',type=Path);s.add_argument('--output',type=Path,required=True);s.set_defaults(run=selfradio)
    s=subs.add_parser('radio');s.add_argument('--station',type=Path,required=True);s.add_argument('--timing',type=Path,required=True);s.add_argument('--station-before',type=Path);s.add_argument('--timing-before',type=Path);s.add_argument('--id',required=True);s.add_argument('--title',required=True);s.add_argument('--title-zh');s.add_argument('--output',type=Path,required=True);s.set_defaults(run=radio)
    args=p.parse_args()
    try:args.run(args)
    except (ValueError,OSError,KeyError,struct.error) as e:p.exit(2,str(e)+'\n')
