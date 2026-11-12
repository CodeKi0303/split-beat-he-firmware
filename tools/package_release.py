"""Bundle verified images and source revision; does not program a device."""
from pathlib import Path
import argparse,hashlib,json,shutil,subprocess,zipfile,uuid
from source_evidence import verify_source
from firmware_artifacts import DISPLAY_VERSION, validate_build_info
ROOT=Path(__file__).resolve().parents[1]
PUBLIC_ROOT_DOCS=('README.md','PROTOCOL.md','LICENSE','THIRD_PARTY.md','default-bindings.json')
PUBLIC_C86_DOCS=('FIRMWARE-UPDATE.md','HALL-CURVE.md','HALL-FILTER.md',
                 'MCP-CONTROL.md','RETRIGGER-BEFORE-RELEASE.md')

def public_documents(root,hardware,app_only):
    """An explicit public allowlist: never package local validation histories."""
    docs=[(root/name,Path(name)) for name in PUBLIC_ROOT_DOCS]
    if hardware=='c86':
        docs.extend((root/'docs'/name,Path('docs')/name) for name in PUBLIC_C86_DOCS)
        docs.append((root/'C86-FIRST-FLASH.md',Path('C86-FIRST-FLASH.md')))
        start=root/'docs/FIRMWARE-UPDATE.md' if app_only else root/'C86-FIRST-FLASH.md'
        docs.append((start,Path('START-HERE-C86.md')))
    missing=sorted({str(source.relative_to(root)) for source,_ in docs if not source.is_file()})
    if missing:raise RuntimeError('Required public package documents are missing: '+', '.join(missing))
    return docs

def copy_public_documents(documents,out):
    for source,relative in documents:
        dest=out/relative;dest.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(source,dest)

def prepare_output(root,name):
    """Preserve incomplete previous output and start clean; never delete it."""
    parent=(root/'dist').resolve();parent.mkdir(parents=True,exist_ok=True)
    out=parent/name
    if out.resolve().parent!=parent:raise ValueError('Package path must stay directly within dist')
    if out.exists():
        previous=parent/(name+'.previous-'+uuid.uuid4().hex[:12])
        out.rename(previous)
    out.mkdir()
    return out

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--hardware',choices=['c77','c85','c86'],default='c77')
    ap.add_argument('--role',choices=['left','right','both'],default='both')
    ap.add_argument('--scan-hz',type=int,choices=[16000,64000,75000,96000])
    ap.add_argument('--adc-dual64',action='store_true')
    ap.add_argument('--adc-dual96-12bit',action='store_true')
    ap.add_argument('--adc-capture-diag',action='store_true')
    ap.add_argument('--adc-isr-probe',action='store_true')
    ap.add_argument('--adc-async64',action='store_true')
    ap.add_argument('--adc-async96',action='store_true')
    ap.add_argument('--adc-dual64-matched40',action='store_true');a=ap.parse_args()
    if a.scan_hz in (64000,75000,96000) and a.hardware!='c86':ap.error('High-rate scanning only applies to C86')
    if a.scan_hz==75000 and a.role!='left':ap.error('75kHz is a left-only experiment; specify --role left')
    if a.scan_hz==96000 and a.role!='left' and not a.adc_async96:
        ap.error('96kHz is a left-only experiment without --adc-async96; specify --role left')
    scan_hz=a.scan_hz or (64000 if a.hardware=='c86' else 16000)
    if a.adc_async64 and (a.hardware!='c86' or a.role!='left' or a.scan_hz!=64000 or
                          a.adc_dual64 or a.adc_dual96_12bit or a.adc_capture_diag or
                          a.adc_dual64_matched40 or a.adc_isr_probe or a.adc_async96):
        ap.error('--adc-async64 requires --hardware c86 --role left --scan-hz 64000 and no other ADC mode flags')
    if a.adc_async96 and (a.hardware!='c86' or a.scan_hz!=96000 or
                          a.adc_dual64 or a.adc_dual96_12bit or a.adc_capture_diag or
                          a.adc_dual64_matched40 or a.adc_isr_probe or a.adc_async64):
        ap.error('--adc-async96 requires --hardware c86 --scan-hz 96000 and no other ADC mode flags')
    if a.adc_isr_probe and not a.adc_capture_diag:
        ap.error('--adc-isr-probe requires --adc-capture-diag')
    if a.adc_capture_diag and (a.hardware!='c86' or a.role!='left' or scan_hz!=64000 or a.adc_dual64 or a.adc_dual96_12bit):
        ap.error('--adc-capture-diag requires C86 left64000Hz without --adc-dual64 or --adc-dual96-12bit')
    if a.adc_dual64_matched40 and not a.adc_capture_diag:
        ap.error('--adc-dual64-matched40 requires --adc-capture-diag')
    if a.adc_dual96_12bit and (a.hardware!='c86' or a.role!='left' or scan_hz!=96000 or a.adc_dual64):
        ap.error('--adc-dual96-12bit requires --hardware c86 --role left --scan-hz 96000 without --adc-dual64')
    if a.adc_dual64 and (a.hardware!='c86' or a.role!='left' or scan_hz!=64000):
        ap.error('--adc-dual64 requires --hardware c86 --role left and 64000 Hz')
    dirty=subprocess.check_output(['git','status','--porcelain'],cwd=ROOT,text=True)
    if dirty:raise RuntimeError('Commit verified source before packaging')
    commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
    stage='usb-update-candidate' if a.hardware=='c86' else 'prehardware'
    version=DISPLAY_VERSION
    recovery='-scan16k' if a.hardware=='c86' and scan_hz==16000 else ('-scan96k-left' if scan_hz==96000 else '')
    if a.adc_async64:recovery='-scan64k-async-left'
    elif a.adc_async96:recovery=f'-scan96k-async-{a.role}'
    elif a.adc_isr_probe:recovery='-scan64k-dual40-isr-probe-left' if a.adc_dual64_matched40 else '-scan64k-single-isr-probe-left'
    elif a.adc_capture_diag:recovery='-scan64k-dual40-diag-left' if a.adc_dual64_matched40 else '-scan64k-single-diag-left'
    elif scan_hz==75000:recovery='-scan75k-left'
    elif a.adc_dual96_12bit:recovery='-scan96k-dual12-left'
    elif a.adc_dual64:recovery='-scan64k-dual-left'
    elif a.role!='both' and scan_hz!=96000:recovery+=f'-{a.role}'
    app_only=scan_hz in (75000,96000) or a.adc_dual64 or a.adc_capture_diag or a.adc_async64
    documents=public_documents(ROOT,a.hardware,app_only)
    name=f'{a.hardware}-v{version}-{stage}{recovery}';out=prepare_output(ROOT,name)
    base=ROOT/'build' if a.hardware=='c77' else ROOT/'build'/a.hardware
    if a.hardware=='c86' and scan_hz==16000:base=base/'scan16k'
    elif scan_hz==75000:base=base/'scan75k'
    elif a.adc_dual96_12bit:base=base/'scan96k-dual12'
    elif a.adc_async96:base=base/'scan96k-async'
    elif scan_hz==96000:base=base/'scan96k'
    elif a.adc_dual64:base=base/'scan64k-dual'
    elif a.adc_async64:base=base/'scan64k-async'
    elif a.adc_isr_probe:base=base/('scan64k-dual40-isr-probe' if a.adc_dual64_matched40 else 'scan64k-single-isr-probe')
    elif a.adc_capture_diag:base=base/('scan64k-dual40-diag' if a.adc_dual64_matched40 else 'scan64k-single-diag')
    verification=json.loads((base/'image-verification.json').read_text())
    roles=['left','right'] if a.role=='both' else [a.role]
    for role in roles:
        source=base/role;dest=out/role;dest.mkdir(exist_ok=True)
        info=json.loads((source/'build-info.json').read_text())
        assert info['hardware_profile']==a.hardware
        assert info['scan_target_hz']==scan_hz and info['version']==version
        assert info['adc_dual64']==(a.adc_dual64 or a.adc_dual64_matched40 or a.adc_async64)
        assert info['adc_async64']==a.adc_async64
        assert info['adc_async96']==a.adc_async96
        assert info['adc_dual96_12bit']==a.adc_dual96_12bit
        assert info['adc_capture_diag']==a.adc_capture_diag
        cpu_profile=a.adc_capture_diag and not a.adc_isr_probe
        assert info['cpu_profile']==cpu_profile
        assert info['adc_irq_probe']==a.adc_isr_probe
        assert info['adc_dual64_matched40']==a.adc_dual64_matched40
        validate_build_info(info,(source/f'{a.hardware}-{role}-app.bin').read_bytes(),a.hardware,role)
        verify_source(info)
        assert verification[role]['version']==info['version'] and verification[role]['version_number']==info['version_number']
        assert verification[role]['source_tree_sha256']==info['source_tree_sha256']
        assert verification[role]['files']==info['files'],'Image verification belongs to another build'
        assert verification[role].get('adc_dual96_12bit',False)==a.adc_dual96_12bit
        assert verification[role].get('adc_capture_diag',False)==a.adc_capture_diag
        assert verification[role]['cpu_profile']==cpu_profile
        assert verification[role]['adc_irq_probe']==a.adc_isr_probe
        assert verification[role]['adc_async64']==a.adc_async64
        assert verification[role]['adc_async96']==a.adc_async96
        assert verification[role].get('adc_dual64_matched40',False)==a.adc_dual64_matched40
        if a.adc_capture_diag:
            assert verification[role]['scan_target_hz']==64000 and verification[role]['adc_resolution_bits']==12
            assert info['adc_resolution_bits']==12 and info['adc_effective_clock_hz']==40000000 and info['adc_sample_cycles']==64.5
            assert info['adc_instances']==([1,2] if a.adc_dual64_matched40 else [1])
            assert info['adc_ranks']==([4,3] if a.adc_dual64_matched40 else [7])
            assert info['dma_frames_per_irq']==1 and info['publication_target_hz']==64000 and info['filter_samples']==8
            assert info['sample_frame_bytes']==48 and info['filter_execution']=='per-scan IRQ running sum'
        if a.adc_dual96_12bit:
            assert verification[role]['scan_target_hz']==96000 and verification[role]['adc_resolution_bits']==12
            assert info['adc_instances']==[1,2] and info['adc_resolution_bits']==12 and info['adc_ranks']==[4,3]
            assert info['adc_effective_clock_hz']==30000000 and info['adc_sample_cycles']==64.5
            assert info['dma_frames_per_irq']==2 and info['publication_target_hz']==48000 and info['filter_samples']==8
            assert info['sample_frame_bytes']==48 and info['filter_execution']=='per-batch IRQ running sum over both scans'
        if a.adc_dual64:
            assert verification[role]['adc_dual64'] is True
            assert info['adc_instances']==[1,2] and info['adc_resolution_bits']==12
            assert info['adc_effective_clock_hz']==30000000 and info['adc_ranks']==[4,3]
        if a.adc_async64 or a.adc_async96:
            assert verification[role]['adc_dual64']==a.adc_async64
            assert info['adc_resolution_bits']==12 and info['adc_instances']==[1,2] and info['adc_ranks']==([8,5] if a.adc_async96 else [4,3])
            assert info['adc_effective_clock_hz']==40000000 and info['adc_sample_cycles']==64.5
            assert info['dma_scans_per_bank']==32 and info['dma_frames_per_irq']==16
            assert info['dma_irq_target_hz_per_bank']==scan_hz//16 and info['publication_target_hz'] is None
            assert info['publication_mode']=='foreground latest completed per bank'
            assert info['filter_samples']==8 and info['filter_execution']=='foreground per-ADC eight-sample average'
            assert info['sample_frame_bytes']==48 and info['hall_decision_precision']=='legacy integer ADC'
        if a.adc_async96 and role=='right':
            assert info['split_reply_bytes']==32 and info['split_hall_input']=='12-bit configured per-channel average'
        if scan_hz==75000:
            assert verification[role]['scan_target_hz']==75000 and verification[role]['adc_dual64'] is False
            assert info['adc_instances']==[1] and info['adc_resolution_bits']==12 and info['adc_ranks']==[7]
            assert info['adc_effective_clock_hz']==40000000 and info['adc_sample_cycles']==64.5
            assert info['dma_frames_per_irq']==1 and info['publication_target_hz']==75000 and info['filter_samples']==8
        for relative,expected in info.get('hardware_evidence_sha256',{}).items():
            assert hashlib.sha256((ROOT/relative).read_bytes()).hexdigest()==expected,relative
            destination=out/relative;destination.parent.mkdir(parents=True,exist_ok=True)
            shutil.copy2(ROOT/relative,destination)
        candidates=[*source.glob('*.bin'),*source.glob('*.hex'),*source.glob('*.elf'),*source.glob('*.fw.json')]
        if scan_hz in (75000,96000) or a.adc_dual64 or a.adc_capture_diag or a.adc_async64:
            # This incremental experiment never distributes a replacement
            # permanent bootloader or a whole-device factory image.
            candidates=[f for f in candidates if f.name.startswith(f'c86-{role}-app.') or f.name==f'c86-{role}.elf']
        for f in candidates:
            assert hashlib.sha256(f.read_bytes()).hexdigest()==info['files'][f.name]
            shutil.copy2(f,dest/f.name)
        shutil.copy2(source/'build-info.json',dest/'build-info.json')
        if scan_hz in (75000,96000) or a.adc_dual64 or a.adc_capture_diag or a.adc_async64:
            allowed={f.name for f in candidates}|{'build-info.json'}
            assert all(p.is_file() and p.name in allowed for p in dest.iterdir()),'Unexpected files in app-only package; use a clean release destination'
    copy_public_documents(documents,out)
    if a.adc_async96:
        selected_verification={role:verification[role] for role in roles}
        (out/'image-verification.json').write_text(json.dumps(selected_verification,indent=2)+'\n',encoding='utf-8')
    else:
        shutil.copy2(base/'image-verification.json',out/'image-verification.json')
    files={p.relative_to(out).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in out.rglob('*') if p.is_file() and p.name!='release.json'}
    report={'version':f'{version}-{stage}{recovery}','hardware_profile':a.hardware,'source_commit':commit,
            'source_tree_sha256':info['source_tree_sha256'],'hardware_validated':False,
            'scan_target_hz':scan_hz,'cpu_profile':cpu_profile,'adc_irq_probe':a.adc_isr_probe,'adc_async64':a.adc_async64,'adc_async96':a.adc_async96,
            'initial_flash':f'left/right {a.hardware}-*-factory.hex using SWD on each MCU','files':files}
    if a.hardware=='c86':report.update({'subsequent_update':'Role-matched c86-*-app.fw.json through permanent updater',
                                      'permanent_updater_initial_install_requires_swd':True,
                                      'power_loss_hardware_qualified':False})
    if scan_hz==96000 and not a.adc_dual96_12bit and not a.adc_async96:
        report.update({'roles':['left'],'initial_flash':'Not included: existing C86 permanent updater required',
                       'right_app_changed':False,'settings_migration':False,
                       'adc_resolution_bits':14,'filter_samples':8,'decision_units':'Q5 of legacy12-bit ADC',
                       'dma_frames_per_irq':2,'publication_target_hz':48000,
                       'filter_execution':'foreground snapshot of all eight latest ADC frames',
                       'installation_status':'PREPARED_NOT_INSTALLED'})
    if a.adc_dual96_12bit:
        report.update({'roles':['left'],'initial_flash':'Not included: existing C86 permanent updater required',
                       'right_app_changed':False,'settings_migration':False,'adc_dual64':False,'adc_dual96_12bit':True,
                       'adc_instances':[1,2],'adc_ranks':[4,3],'adc_effective_clock_hz':30000000,
                       'adc_resolution_bits':12,'adc_sample_cycles':64.5,'filter_samples':8,'sample_frame_bytes':48,
                       'decision_units':'legacy integer12-bit ADC','dma_frames_per_irq':2,
                       'publication_target_hz':48000,'filter_execution':'per-batch IRQ running sum over both scans',
                       'installation_status':'PREPARED_NOT_INSTALLED'})
    if a.adc_dual64:
        report.update({'roles':['left'],'initial_flash':'Not included: existing C86 permanent updater required',
                       'right_app_changed':False,'settings_migration':False,'adc_dual64':True,
                       'adc_instances':[1,2],'adc_ranks':[4,3],'adc_effective_clock_hz':30000000,
                       'adc_resolution_bits':12,'adc_sample_cycles':64.5,'filter_samples':8,
                       'decision_units':'legacy integer12-bit ADC','dma_frames_per_irq':1,
                       'publication_target_hz':64000,'filter_execution':'per-scan IRQ running sum',
                       'installation_status':'PREPARED_NOT_INSTALLED'})
    if scan_hz==75000:
        report.update({'roles':['left'],'initial_flash':'Not included: existing C86 permanent updater required',
                       'right_app_changed':False,'settings_migration':False,'adc_dual64':False,
                       'adc_instances':[1],'adc_ranks':[7],'adc_effective_clock_hz':40000000,
                       'adc_resolution_bits':12,'adc_sample_cycles':64.5,'filter_samples':8,
                       'decision_units':'legacy integer12-bit ADC','dma_frames_per_irq':1,
                       'publication_target_hz':75000,'filter_execution':'per-scan IRQ running sum',
                       'installation_status':'PREPARED_NOT_INSTALLED'})
    if a.adc_capture_diag:
        report.update({'roles':['left'],'initial_flash':'Not included: existing C86 permanent updater required',
                       'right_app_changed':False,'settings_migration':False,'adc_dual64':a.adc_dual64_matched40,
                       'adc_dual64_matched40':a.adc_dual64_matched40,'adc_capture_diag':True,
                       'adc_dual96_12bit':False,'adc_effective_clock_hz':40000000,
                       'adc_instances':[1,2] if a.adc_dual64_matched40 else [1],
                       'adc_ranks':[4,3] if a.adc_dual64_matched40 else [7],
                       'adc_resolution_bits':12,'adc_sample_cycles':64.5,'filter_samples':8,'sample_frame_bytes':48,
                       'decision_units':'legacy integer12-bit ADC','dma_frames_per_irq':1,
                       'publication_target_hz':64000,'filter_execution':'per-scan IRQ running sum',
                       'installation_status':'PREPARED_NOT_INSTALLED'})
    if a.adc_async64 or a.adc_async96:
        report.update({'roles':roles,'initial_flash':'Not included: existing C86 permanent updater required',
                       'right_app_changed':'right' in roles,'settings_migration':False,'adc_dual64':a.adc_async64,
                       'adc_instances':[1,2],'adc_ranks':[8,5] if a.adc_async96 else [4,3],
                       'adc_effective_clock_hz':40000000,
                       'adc_resolution_bits':12,'adc_sample_cycles':64.5,'filter_samples':8,'sample_frame_bytes':48,
                       'decision_units':'legacy integer12-bit ADC','dma_scans_per_bank':32,
                       'dma_frames_per_irq':16,'dma_irq_target_hz_per_bank':scan_hz//16,
                       'publication_target_hz':None,'publication_mode':'foreground latest completed per bank',
                       'filter_execution':'foreground per-ADC eight-sample average',
                       'adc_sequence_meaning':'minimum completed scan cursor across both ADC banks',
                       'installation_status':'PREPARED_NOT_INSTALLED'})
        if 'right' in roles:
            report.update({'split_reply_bytes':32,'split_hall_input':'12-bit configured per-channel average'})
    (out/'release.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    archive=ROOT/'dist'/f'{name}.zip'
    with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as z:
        for p in sorted(out.rglob('*')):
            if p.is_file():z.write(p,p.relative_to(ROOT/'dist'))
    print(json.dumps({'archive':str(archive),'source_commit':commit,'bytes':archive.stat().st_size,'sha256':hashlib.sha256(archive.read_bytes()).hexdigest()}))
if __name__=='__main__':main()
