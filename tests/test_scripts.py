"""Execute the actual C script validator/installer against disposable RPFs.

Host tests use a C compiler and zlib (Windows: Zig, Linux: cc). Only libnx mount/commit calls are stubs.
No installed game files are opened. NRO/console behavior is a separate check.
"""
import argparse, ctypes, hashlib, json, os, struct, subprocess, tempfile, zlib
from pathlib import Path

def compile_library(root, output, zlib_source):
    output.mkdir(parents=True, exist_ok=True)
    import shlex
    sources=[root/'source'/n for n in ['gc_nsc.c','gc_script.c','gc_rpf.c','gc_package.c','gc_vehicle.c']]+[root/'tests/support/host.c']
    paths=output/'host_paths.h'
    paths.write_text('#define GC_PACKAGE_GAME "'+(output/'fixture-game').resolve().as_posix()+'"\n#define GC_PACKAGE_HOME "'+(output/'fixture-packages').resolve().as_posix()+'"\n'+
                     '#define GC_PACKAGE_RENAME gc_host_rename\nint gc_host_rename(const char *,const char *);\n')
    compiler=shlex.split(os.environ.get('CC','cc'),posix=os.name!='nt')
    library=output/('toolbox_tests.dll' if os.name=='nt' else 'toolbox_tests.so')
    args=compiler+['-shared','-O2','-std=c11','-D_DEFAULT_SOURCE','-include',str(paths.resolve()),'-I'+str(root/'tests/support')]
    if os.name=='nt':
        if zlib_source is None:raise RuntimeError('Windows tests require --zlib-source')
        sources += [zlib_source/n for n in ['adler32.c','crc32.c','compress.c','deflate.c','inflate.c','inffast.c','inftrees.c','trees.c','zutil.c']]
        args += ['-I'+str(zlib_source)]
        exports=['gc_nsc_validate','gc_nsc_decode_validate','gc_script_install','gc_script_uninstall','gc_host_set_archive','gc_package_read','gc_package_install','gc_package_restore','gc_package_state','gc_package_recover','gc_host_fail_rename','gc_vehicle_inspect']
        definitions=output/'exports.def'
        definitions.write_text('EXPORTS\n'+'\n'.join(exports)+'\n')
        args += [str(definitions.resolve())]
    else:args += ['-fPIC','-Wno-deprecated-declarations']
    args += [str(x) for x in sources]+['-o',str(library.resolve())]+(['-lbcrypt'] if os.name=='nt' else ['-lz','-lcrypto'])
    run=subprocess.run(args,capture_output=True,text=True,encoding="utf-8",errors="replace")
    (output/'compile.log').write_text(run.stdout+run.stderr)
    if run.returncode:raise RuntimeError(run.stdout+run.stderr)
    return ctypes.CDLL(str(library.resolve()))


def joaat(text):
    h=0
    for b in text.lower().encode():h=(h+b)&0xffffffff;h=(h+(h<<10))&0xffffffff;h^=h>>6
    h=(h+(h<<3))&0xffffffff;h^=h>>11;return (h+(h<<15))&0xffffffff

def payload(name):
    p=bytearray(256);struct.pack_into('<Q',p,0,0x7ff7bad5b3a8)
    struct.pack_into('<Q',p,0x10,0x500000b0);struct.pack_into('<II',p,0x18,0x94c75b53,4)
    struct.pack_into('<I',p,0x58,joaat(name));struct.pack_into('<Q',p,0x60,0x50000080)
    p[128:128+len(name)+1]=name.encode()+b'\0';struct.pack_into('<Q',p,176,0x500000c8)
    p[200:204]=b'CODE';return bytes(p)

def flat_rpf(files, names_len=0, offset=512):
    names=bytearray(b'\0');records=[];pos=offset;data=bytearray(offset)
    for name,raw,compressed in files:
        nameoff=len(names);names+=name.encode()+b'\0'
        content=zlib.compress(raw)[2:-4] if compressed else raw
        record=(nameoff | ((len(content) if compressed else 0)<<16) | ((pos//512)<<40))
        records.append(struct.pack('<QII',record,len(raw),0))
        data.extend(content);data.extend(bytes((-len(data))%512));pos=len(data)
    if names_len: names+=bytes(names_len-len(names))
    ec=len(records)+1
    head=struct.pack('<4I',0x52504637,ec,len(names),0x4e45504f)+struct.pack('<4I',0,0x7fffff00,1,len(records))+b''.join(records)+names
    assert len(head)<=offset;data[:len(head)]=head;return bytes(data)

def read_flat(data, base=0):
    magic,ec,nl,enc=struct.unpack_from('<4I',data,base)
    assert (magic,enc)==(0x52504637,0x4e45504f)
    first,count=struct.unpack_from('<2I',data,base+24);assert first==1 and count==ec-1
    names=data[base+16+ec*16:base+16+ec*16+nl];out={}
    for i in range(1,ec):
        q,size,flags=struct.unpack_from('<QII',data,base+16+i*16);assert flags==0 and q>>63==0
        name=names[q&65535:].split(b'\0')[0].decode();packed=(q>>16)&0xffffff;at=base+((q>>40)&0x7fffff)*512
        content=data[at:at+(packed or size)]
        out[name]=zlib.decompress(content,-15) if packed else content
        assert len(out[name])==size
    return out

def test(root, output, zlib_source):
    lib=compile_library(root,output,zlib_source);err=ctypes.create_string_buffer(512);cases=[]
    for name in ('gc_nsc_validate','gc_nsc_decode_validate'):
        getattr(lib,name).argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_char_p,ctypes.c_void_p,ctypes.c_size_t]
    def check(label,ok):
        if not ok:raise AssertionError(label+' '+err.value.decode('utf-8','replace'))
        cases.append(label)
    def validate(data,name):return lib.gc_nsc_decode_validate(data,len(data),name.encode(),err,512)
    for path in (root/'romfs').rglob('*.nsc'):
        name='simple_zombies.nsc' if path.name=='simple_zombies_combo.nsc' else path.name
        check('bundled script payload '+str(path.relative_to(root)),validate(path.read_bytes(),name)==0)
    valid=payload('example')
    check('raw script validated',validate(valid,'example.nsc')==0)
    check('renamed script rejected',validate(valid,'renamed.nsc')<0)
    check('PC intermediate rejected',validate(valid,'example.pc.nsc')<0)
    for offset,value in [(0,0),(0x60,0x50ffffff),(0x10,0x50fffff0),(0xb0,0x50fffff0),(0x68,0x50fffff0)]:
        bad=bytearray(valid);struct.pack_into('<Q',bad,offset,value)
        if offset==0x68:struct.pack_into('<I',bad,0x70,500)
        check('out-of-range field rejected '+hex(offset),validate(bytes(bad),'example.nsc')<0)
    bad=bytearray(valid);struct.pack_into('<I',bad,0x58,0)
    check('corrupt name hash rejected',validate(bytes(bad),'example.nsc')<0)
    envelope=struct.pack('<4I',0x37435352,12,0x80,0xc0000000)
    packed=zlib.compress(valid)[2:-4]
    check('RSC7 trial-decodes compressed stream',validate(envelope+packed,'example.nsc')==0)
    check('RSC7 trial-decodes uncompressed stream',validate(envelope+valid,'example.nsc')==0)
    check('truncated RSC7 rejected',validate(envelope+packed[:-2],'example.nsc')<0)
    check('RSC7 trailing garbage rejected',validate(envelope+packed+b'bad','example.nsc')<0)
    with tempfile.TemporaryDirectory(dir=output) as folder:
        folder=Path(folder);target=folder/'update2.rpf';source=folder/'added.nsc';source.write_bytes(payload('added'))
        inner=flat_rpf([('old.nsc',payload('old'),True)],names_len=464)
        outer=flat_rpf([('script_rel.rpf',inner,False),('unrelated.bin',bytes(range(256))*4,False)],offset=1024)
        target.write_bytes(outer);lib.gc_host_set_archive(str(target).encode())
        check('new script installs through original C API',lib.gc_script_install(str(source).encode(),b'added.nsc',err,512)==0)
        changed=read_flat(target.read_bytes()); scripts=read_flat(changed['script_rel.rpf'])
        check('header growth preserves original script through relocation',scripts['old.nsc']==payload('old'))
        check('new entry is reachable from correct root count',scripts['added.nsc']==payload('added'))
        check('outer neighboring payload is unchanged',changed['unrelated.bin']==bytes(range(256))*4)
        check('first script can be removed without deleting root',lib.gc_script_uninstall(b'old.nsc',err,512)==0)
        changed=read_flat(target.read_bytes());check('uninstall keeps root first-child index and moves name pool',read_flat(changed['script_rel.rpf'])=={'added.nsc':payload('added')})
        before=target.read_bytes();source.write_bytes(valid)
        check('name mismatch rejected before archive mutation',lib.gc_script_install(str(source).encode(),b'added.nsc',err,512)<0 and target.read_bytes()==before)
    report=dict(passed=len(cases),cases=cases,console_verified=False)
    (output/'script-tests.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(script_checks=len(cases))))

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path,required=True);parser.add_argument('--zlib-source',type=Path)
    args=parser.parse_args();test(Path(__file__).resolve().parents[1],args.output.resolve(),args.zlib_source.resolve() if args.zlib_source else None)
