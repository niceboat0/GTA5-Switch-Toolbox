import argparse,json,os,shlex,subprocess
from pathlib import Path
def test(root,out):
    out.mkdir(parents=True,exist_ok=True)
    installed=out/'installed';imports=out/'imports'
    for parent,names in [(installed,[f'car{i:02}' for i in range(12)]),(imports,['car00','new01','new02'])]:
        for name in names:
            folder=parent/name;folder.mkdir(parents=True,exist_ok=True);(folder/'dlc.rpf').write_bytes(b'fixture')
    compiler=shlex.split(os.environ.get('CC','cc'),posix=os.name!='nt')
    exe=out/('catalog-test.exe' if os.name=='nt' else 'catalog-test')
    args=compiler+['-std=c11','-D_DEFAULT_SOURCE','-O2','-UNDEBUG','-Wall','-Wextra','-Werror','-Wno-unused-function','-I'+str(root/'tests/support'),str(root/'tests/dlc_catalog_test.c'),'-o',str(exe)]
    subprocess.run(args,check=True)
    subprocess.run([str(exe.resolve()),str(installed.resolve()),str(imports.resolve())],check=True)
    (out/'catalog-tests.json').write_text(json.dumps({'passed':True,'console_verified':False}))
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args();test(Path(__file__).resolve().parents[1],a.output.resolve())
