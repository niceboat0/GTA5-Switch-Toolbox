"""Exercise the actual bounded RPF vehicle inspector using disposable archives."""
import argparse,ctypes,json,struct,zlib
from pathlib import Path
from test_scripts import compile_library,flat_rpf
class Vehicle(ctypes.Structure):
    _fields_=[('name',ctypes.c_char*96),('flags',ctypes.c_uint)]
class Report(ctypes.Structure):
    _fields_=[('count',ctypes.c_int),('partial',ctypes.c_int),('archives',ctypes.c_int),('vehicles',Vehicle*512)]
def test(root,out,zlib_source):
    lib=compile_library(root,out,zlib_source);err=ctypes.create_string_buffer(512);r=Report();cases=[]
    lib.gc_vehicle_inspect.argtypes=[ctypes.c_char_p,ctypes.POINTER(Report),ctypes.c_void_p,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_size_t]
    path=out/'fixture.rpf'
    def inspect(data):
        path.write_bytes(data);return lib.gc_vehicle_inspect(str(path).encode(),ctypes.byref(r),None,None,err,512)
    def names():return {v.name.decode():v.flags for v in r.vehicles[:r.count]}
    def check(label,ok):
        assert ok,label+' '+err.value.decode('utf-8','replace');cases.append(label)
    inner=flat_rpf([('Adder.nft',b'x',False),('adder_hi.nft',b'y',False),('zentorno.yft',b'x',False),('ZENTORNO_HI.yft',b'y',False)])
    check('base and high-detail models deduplicate case-insensitively',inspect(inner)==0 and names()=={'adder':3,'zentorno':3})
    outer=flat_rpf([('vehicles.rpf',inner,False)])
    check('uncompressed nested vehicle archive is traversed',inspect(outer)==0 and r.archives==2 and names()=={'adder':3,'zentorno':3})
    compressed=flat_rpf([('vehicles.rpf',inner,True)])
    check('compressed nested vehicle archive is decoded',inspect(compressed)==0 and r.archives==2 and r.count==2)
    mixed=flat_rpf([('one.nft',b'x',False),('different_hi.nft',b'x',False)])
    check('equal totals still expose mismatched per-vehicle pairs',inspect(mixed)==0 and names()=={'one':1,'different':2})
    check('duplicates across nested archives merge',inspect(flat_rpf([('a.rpf',inner,False),('b.rpf',inner,False)]))==0 and r.count==2)
    check('nonvehicle archive produces an empty report',inspect(flat_rpf([('track.awc',b'x',False)]))==0 and r.count==0 and not r.partial)
    bad=bytearray(inner);struct.pack_into('<I',bad,8,0xfffffff)
    check('corrupt header is refused before allocation',inspect(bad)<0)
    bad=bytearray(inner);struct.pack_into('<H',bad,32,65535)
    check('bad name offsets mark an incomplete report',inspect(bad)==0 and r.partial)
    check('invalid nested archive is visible as incomplete',inspect(flat_rpf([('bad.rpf',b'broken',False)]))==0 and r.partial)
    deep=inner
    for i in range(6):deep=flat_rpf([('nested.rpf',deep,False)])
    check('recursion is bounded and reported',inspect(deep)==0 and r.partial)
    check('truncated header is refused',inspect(b'RPF7')<0)
    path.write_bytes(outer)
    with path.open('r+b') as f:f.truncate(600*1024*1024)
    check('large sparse pack needs only headers and names',lib.gc_vehicle_inspect(str(path).encode(),ctypes.byref(r),None,None,err,512)==0 and r.count==2)
    path.unlink()
    (out/'vehicle-tests.json').write_text(json.dumps({'passed':len(cases),'cases':cases,'console_verified':False},indent=2))
    print(json.dumps({'vehicle_checks':len(cases)}))
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--zlib-source',type=Path);a=p.parse_args()
    test(Path(__file__).resolve().parents[1],a.output.resolve(),a.zlib_source.resolve() if a.zlib_source else None)
