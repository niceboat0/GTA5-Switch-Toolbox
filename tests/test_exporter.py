"""Publisher exports: paired assets, safe metadata, actual copied payload hashes."""
import argparse,contextlib,hashlib,importlib.util,io,json,tempfile
from pathlib import Path
from types import SimpleNamespace
from test_scripts import flat_rpf

def test(output):
    root=Path(__file__).resolve().parents[1]
    spec=importlib.util.spec_from_file_location('exporter',root/'tools/export_package.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    cases=[]
    def check(name,okay):
        assert okay,name;cases.append(name)
    def rejects(name,call):
        try:call()
        except (ValueError,FileExistsError):check(name,True)
        else:check(name,False)
    output.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output) as temp:
        folder=Path(temp);station=folder/'RADIO_TEST.rpf';timing=folder/'audio_rel.rpf'
        station.write_bytes(flat_rpf([('track.awc',b'new audio',False)]))
        timing.write_bytes(flat_rpf([('sounds.dat54.rel',b'new timing',False)]))
        args=SimpleNamespace(station=station,timing=timing,station_before=None,timing_before=None,
                             id='test-radio',title='Test Radio',title_zh='电台测试',output=folder/'package')
        with contextlib.redirect_stdout(io.StringIO()):module.radio(args)
        lines=(args.output/'package.txt').read_text(encoding='utf-8').splitlines()
        check('two outputs exported as one radio package',len([l for l in lines if l.startswith('FILE|')])==2)
        for line in lines[5:]:
            _,target,entry,source,digest,before=line.split('|')
            check('copied payload checksum '+target,hashlib.sha256((args.output/source).read_bytes()).hexdigest()==digest)
        check('Chinese title preserved','title_zh|电台测试' in lines)
        rejects('existing export preserved',lambda:module.radio(args))
        for bad in ['../escape','Uppercase','a'*64]:
            rejects('unsafe id '+bad,lambda:module.create(folder/'bad',bad,'Title','标题','radio',[]))
        for bad in ['','line\nline','title|value','中'*32]:
            rejects('unsafe title '+repr(bad),lambda:module.create(folder/'bad','valid',bad,'标题','radio',[]))
        args.output=folder/'bad';station.write_bytes(flat_rpf([('not-audio.bin',b'none',False)]))
        rejects('station without AWC refused',lambda:module.radio(args))
        station.write_bytes(flat_rpf([('track.awc',b'audio',False)]));timing.write_bytes(flat_rpf([('game.dat151.rel',b'wrong',False)]))
        rejects('dat151 is not substituted for dat54 timing',lambda:module.radio(args))
        station.write_bytes(station.read_bytes()[:30])
        rejects('truncated archive refused',lambda:module.radio(args))
    report={'passed':len(cases),'cases':cases}
    (output/'exporter-tests.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({'exporter_checks':len(cases)}))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);test(p.parse_args().output.resolve())
