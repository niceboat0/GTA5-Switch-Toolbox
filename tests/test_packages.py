"""Actual C package installer: paired commits, preservation and rollback."""
import argparse,ctypes,hashlib,json,shutil,sys
from pathlib import Path
from test_scripts import compile_library,flat_rpf,read_flat

class File(ctypes.Structure):
    _fields_=[('mode',ctypes.c_char*8),('target',ctypes.c_char*160),('entry',ctypes.c_char*200),('source',ctypes.c_char*160),('hash',ctypes.c_char*65),('before',ctypes.c_char*65)]
class Pack(ctypes.Structure):
    _fields_=[('folder',ctypes.c_char*512),('id',ctypes.c_char*64),('title',ctypes.c_char*96),('title_zh',ctypes.c_char*96),('kind',ctypes.c_char*16),('count',ctypes.c_int),('files',File*24)]

def test(root,out,zlib_source):
    lib=compile_library(root,out,zlib_source);err=ctypes.create_string_buffer(512);cases=[]
    def check(name,ok):
        if not ok:raise AssertionError(name+' '+err.value.decode('utf-8','replace'))
        cases.append(name)
    game=out/'fixture-game';home=out/'fixture-packages'
    for path in [game,home]:
        if path.exists():
            assert out.resolve() in path.resolve().parents;shutil.rmtree(path)
        path.mkdir()
    payload=home/'test-radio';payload.mkdir();(payload/'station.rpf').write_bytes(flat_rpf([('track.awc',b'new audio',False)]))
    (payload/'audio_rel.rpf').write_bytes(flat_rpf([('sounds.dat54.rel',b'new timing',False)]))
    station=game/'romfs/switch/audio/sfx/radio_test.rpf';timing=game/'romfs/switch/audio/audio_rel.rpf'
    station.parent.mkdir(parents=True);station.write_bytes(b'old radio');timing.write_bytes(b'old timing')
    sha=lambda data:hashlib.sha256(data).hexdigest()
    lines=['GTA5-TOOLBOX-PACK|1','id|test-radio','title|Test radio','kind|radio',
           '|'.join(['FILE','romfs/switch/audio/sfx/radio_test.rpf','','station.rpf',sha((payload/'station.rpf').read_bytes()),sha(b'old radio')]),
           '|'.join(['FILE','romfs/switch/audio/audio_rel.rpf','','audio_rel.rpf',sha((payload/'audio_rel.rpf').read_bytes()),sha(b'old timing')])]
    manifest=payload/'package.txt';manifest.write_text('\n'.join(lines)+'\n')
    p=Pack();check('paired radio manifest loads',lib.gc_package_read(str(payload).encode(),ctypes.byref(p),err,512)==0)
    lib.gc_host_fail_rename(5)
    check('second commit failure rolls back both originals',lib.gc_package_install(ctypes.byref(p),None,None,err,512)<0 and
          station.read_bytes()==b'old radio' and timing.read_bytes()==b'old timing' and not (home/'.pending').exists())
    stage_block=Path(str(timing)+'.toolbox-new');stage_block.mkdir()
    check('staging failure changes neither target',lib.gc_package_install(ctypes.byref(p),None,None,err,512)<0 and
          station.read_bytes()==b'old radio' and timing.read_bytes()==b'old timing')
    stage_block.rmdir()
    check('paired install succeeds',lib.gc_package_install(ctypes.byref(p),None,None,err,512)==0)
    check('audio and timing both installed',station.read_bytes()==(payload/'station.rpf').read_bytes() and timing.read_bytes()==(payload/'audio_rel.rpf').read_bytes())
    check('state checks actual installed payloads',lib.gc_package_state(ctypes.byref(p))==1)
    check('repeated install is idempotent and keeps first backup',lib.gc_package_install(ctypes.byref(p),None,None,err,512)==0 and (home/'.backups/test-radio/0.bin').read_bytes()==b'old radio')
    timing.write_bytes(b'external edit')
    check('restore preserves later third-party changes',lib.gc_package_restore(ctypes.byref(p),err,512)<0 and timing.read_bytes()==b'external edit')
    timing.write_bytes((payload/'audio_rel.rpf').read_bytes())
    check('restore reverts both files',lib.gc_package_restore(ctypes.byref(p),err,512)==0 and station.read_bytes()==b'old radio' and timing.read_bytes()==b'old timing')
    (payload/'audio_rel.rpf').write_bytes(b'corrupt')
    check('corrupt second source stops before any target mutation',lib.gc_package_install(ctypes.byref(p),None,None,err,512)<0 and station.read_bytes()==b'old radio' and timing.read_bytes()==b'old timing')
    for name,text in [('traversal','../evil'),('absolute','/evil'),('drive','C:/evil')]:
        manifest.write_text('\n'.join(lines).replace('station.rpf|',text+'|')+'\n')
        check('unsafe payload rejected '+name,lib.gc_package_read(str(payload).encode(),ctypes.byref(Pack()),err,512)<0)
    manifest.write_text('\n'.join(lines[:-1])+'\n')
    check('audio without timing rejected',lib.gc_package_read(str(payload).encode(),ctypes.byref(Pack()),err,512)<0)
    # Small nested-path RPF fixture proves raw nested archives stay seekable.
    test=home/'selfradio';test.mkdir();before=b'original pause';after=b'patched pause'
    (test/'pause.xml').write_bytes(after)
    archive=game/'romfs/update/update.rpf';archive.parent.mkdir(parents=True)
    # root/common/data/ui chain with five records, preserving unrelated.bin.
    import struct
    names=b'\0common\0data\0ui\0pausemenu.xml\0unrelated.bin\0';ec=6
    records=[struct.pack('<4I',0,0x7fffff00,1,2),struct.pack('<4I',1,0x7fffff00,3,1),
             struct.pack('<QII',29|((1)<<40),4,0),struct.pack('<4I',8,0x7fffff00,4,1),
             struct.pack('<4I',13,0x7fffff00,5,1),struct.pack('<QII',16|((2)<<40),len(before),0)]
    original=bytearray(1536);head=struct.pack('<4I',0x52504637,ec,len(names),0x4e45504f)+b''.join(records)+names
    original[:len(head)]=head;original[512:516]=b'KEEP';original[1024:1024+len(before)]=before;archive.write_bytes(original)
    content=['GTA5-TOOLBOX-PACK|1','id|self-test','title|Self test','kind|selfradio',
             '|'.join(['RPF','romfs/update/update.rpf','common/data/ui/pausemenu.xml','pause.xml',sha(after),sha(before)])]
    (test/'package.txt').write_text('\n'.join(content)+'\n');p=Pack()
    check('Self Radio archive manifest loads',lib.gc_package_read(str(test).encode(),ctypes.byref(p),err,512)==0)
    check('staged RPF entry install succeeds',lib.gc_package_install(ctypes.byref(p),None,None,err,512)==0)
    current=archive.read_bytes();check('unrelated archive content retained',current[512:516]==b'KEEP')
    check('entry state verifies exact updated bytes',lib.gc_package_state(ctypes.byref(p))==1)
    check('raw appended payload and archive are sector-aligned',len(current)%512==0 and struct.unpack_from('<Q',current,16+5*16)[0]>>16&0xffffff==0)
    check('RPF restoration is byte-exact',lib.gc_package_restore(ctypes.byref(p),err,512)==0 and archive.read_bytes()==original)
    corrupt=bytearray(original);struct.pack_into('<I',corrupt,16+5*16+12,1);archive.write_bytes(corrupt)
    check('encrypted binary entry refused without mutation',lib.gc_package_install(ctypes.byref(p),None,None,err,512)<0 and archive.read_bytes()==corrupt)
    archive.write_bytes(original);p.count=25
    check('invalid direct API count refused',lib.gc_package_install(ctypes.byref(p),None,None,err,512)<0 and archive.read_bytes()==original)
    # Simulate power loss after moving the first old file, before all commits.
    backup=home/'.backups/interrupted/0.bin';backup.parent.mkdir(parents=True);backup.write_bytes(b'old interrupted')
    dest=game/'exefs/subsdk4';dest.parent.mkdir();dest.write_bytes(b'new interrupted');stage=Path(str(dest)+'.toolbox-new');stage.write_bytes(b'staged')
    pending=home/'.pending';pending.write_text(f'{dest.as_posix()}|{stage.as_posix()}|{backup.as_posix()}|1|{sha(b"new interrupted")}\n')
    check('interrupted commit recovery restores original',lib.gc_package_recover(err,512)==0 and dest.read_bytes()==b'old interrupted' and not stage.exists())
    backup.write_bytes(b'old interrupted');dest.write_bytes(b'later edits')
    pending.write_text(f'{dest.as_posix()}|{stage.as_posix()}|{backup.as_posix()}|1|{sha(b"new interrupted")}\n')
    check('recovery preserves later edits and backup',lib.gc_package_recover(err,512)<0 and dest.read_bytes()==b'later edits' and backup.read_bytes()==b'old interrupted')
    dest.write_bytes(b'new interrupted');check('recovery can be retried safely',lib.gc_package_recover(err,512)==0 and dest.read_bytes()==b'old interrupted')
    report=dict(passed=len(cases),cases=cases,console_verified=False,sha_fixture='Host BCrypt/OpenSSL; production uses mbedTLS')
    (out/'package-tests.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(dict(package_checks=len(cases))))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--zlib-source',type=Path);a=p.parse_args()
    test(Path(__file__).resolve().parents[1],a.output.resolve(),a.zlib_source.resolve() if a.zlib_source else None)
