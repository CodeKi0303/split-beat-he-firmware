"""Verify built ELF/BIN/HEX structure and hashes, not hardware behavior."""
from pathlib import Path
import argparse,hashlib,json,os,shutil,struct,subprocess
from source_evidence import verify_source
from firmware_artifacts import (APP_BASE, BOOT_BASE, METADATA_BASE, SLOT_END, MAX_IMAGE,
                                validate_build_info,
                                make_factory, metadata_record, validate_bundle, validate_image)
ROOT=Path(__file__).resolve().parents[1]
def ihex(path):
    base=0;memory={}
    for line in path.read_text().splitlines():
        assert line.startswith(':');raw=bytes.fromhex(line[1:]);assert sum(raw)&255==0
        n=raw[0];address=int.from_bytes(raw[1:3],'big');kind=raw[3];data=raw[4:-1];assert n==len(data)
        if kind==4:base=int.from_bytes(data,'big')<<16
        elif kind==2:base=int.from_bytes(data,'big')<<4
        elif kind==0:
            for i,v in enumerate(data):
                target=base+address+i
                assert 0x08000000<=target<0x080c0000,'HEX touches reserved settings/outside flash'
                assert target not in memory or memory[target]==v
                memory[target]=v
        elif kind not in (1,3,5):raise AssertionError('Unknown HEX record')
    return memory
def main():
    ap=argparse.ArgumentParser();ap.add_argument('--toolchain',type=Path);ap.add_argument('--hardware',choices=['c77','c85','c86'],default='c77')
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
    tc=a.toolchain or os.environ.get('ARM_GCC_BIN')
    if tc:tc=Path(tc)
    elif shutil.which('arm-none-eabi-nm'):tc=Path(shutil.which('arm-none-eabi-nm')).parent
    else:tc=ROOT.parent/'.tools/xpack-arm-none-eabi-gcc-15.2.1-1.1/bin'
    nm=tc/('arm-none-eabi-nm.exe' if os.name=='nt' else 'arm-none-eabi-nm')
    result={}
    for role in (['left','right'] if a.role=='both' else [a.role]):
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
        d=base/role;info=json.loads((d/'build-info.json').read_text())
        assert info['hardware_profile']==a.hardware
        assert info['scan_target_hz']==scan_hz
        assert info['adc_dual64']==(a.adc_dual64 or a.adc_dual64_matched40 or a.adc_async64),'Build ADC layout does not match requested profile'
        assert info['adc_async64']==a.adc_async64,'Build asynchronous ADC mode does not match requested profile'
        assert info['adc_async96']==a.adc_async96,'Build96k asynchronous ADC mode does not match requested profile'
        assert info['adc_dual96_12bit']==a.adc_dual96_12bit,'Build96k ADC precision does not match requested profile'
        assert info['adc_capture_diag']==a.adc_capture_diag,'Build capture diagnostics do not match requested profile'
        cpu_profile=a.adc_capture_diag and not a.adc_isr_probe
        assert info['cpu_profile']==cpu_profile,'Build CPU profiling does not match requested profile'
        assert info['adc_irq_probe']==a.adc_isr_probe,'Build ADC IRQ probe does not match requested profile'
        assert info['adc_dual64_matched40']==a.adc_dual64_matched40,'Build ADC clock comparison does not match requested profile'
        verify_source(info)
        for relative,expected in info.get('hardware_evidence_sha256',{}).items():
            assert hashlib.sha256((ROOT/relative).read_bytes()).hexdigest()==expected,relative
        for name,sha in info['files'].items():assert hashlib.sha256((d/name).read_bytes()).hexdigest()==sha,name
        app=(d/f'{a.hardware}-{role}-app.bin').read_bytes();boot=(d/'boot.bin').read_bytes();factory=(d/f'{a.hardware}-{role}-factory.bin').read_bytes()
        validate_build_info(info,app,a.hardware,role)
        assert len(app)<=640*1024 and len(boot)<=128*1024
        for data,origin in [(app,0x08020000),(boot,0x08000000)]:
            sp,pc=struct.unpack_from('<II',data);assert 0x20000000<sp<=0x20020000 and sp%8==0
            assert pc&1 and origin<pc<origin+len(data)
        memory=ihex(d/f'{a.hardware}-{role}-factory.hex')
        if a.hardware=='c86':
            validate_image(app,role);validate_image(boot,role,boot=True)
            assert len(app)<=MAX_IMAGE and factory==make_factory(boot,app,role)
            bundle=json.loads((d/f'c86-{role}-app.fw.json').read_text(encoding='utf-8'))
            validate_bundle(bundle,app,role)
            expected={}
            for origin,data in ((BOOT_BASE,boot),(APP_BASE,app),(METADATA_BASE,metadata_record(app,role))):
                expected.update({origin+i:value for i,value in enumerate(data)})
            assert memory==expected,'Factory HEX missing/extra bytes or incorrect commit metadata'
            assert ihex(d/f'c86-{role}-app.hex')=={APP_BASE+i:value for i,value in enumerate(app)}
            assert max(memory)<SLOT_END and info['app_max_bytes']==MAX_IMAGE
        else:
            assert factory[:len(boot)]==boot and factory[0x20000:]==app and set(factory[len(boot):0x20000])=={255}
            # Legacy objcopy binary fills alignment holes with zero; sparse HEX omits them.
            for i,value in enumerate(app):assert memory.get(0x08020000+i,0)==value
            for i,value in enumerate(boot):assert memory.get(0x08000000+i,0)==value
            assert max(address for address in memory if address>=0x08020000)<0x08020000+len(app)
        symbols={}
        for line in subprocess.check_output([str(nm),'-n',str(d/f'{a.hardware}-{role}.elf')],text=True).splitlines():
            fields=line.split()
            if len(fields)==3:
                try:symbols[fields[2]]=int(fields[0],16)
                except ValueError:pass
        assert symbols['g_pfnVectors']==0x08020000
        assert symbols['__settings_base__']==0x080c0000
        assert ('cpu_profile_command' in symbols)==cpu_profile,'Application CPU profiler symbol does not match requested profile'
        assert ('adc_irq_probe_command' in symbols)==a.adc_isr_probe,'Application ADC IRQ probe symbol does not match requested profile'
        assert ('adc_scan_bank_snapshot' in symbols)==(a.adc_async64 or a.adc_async96),'Application ADC bank API does not match requested profile'
        satellite_async=a.adc_async96 and role=='right'
        assert ('adc_scan_satellite_service' in symbols)==satellite_async,'Application satellite ADC path does not match requested profile'
        if satellite_async:
            assert info['split_reply_bytes']==32 and info['split_hall_input']=='12-bit eight-sample average per channel'
        if a.hardware=='c86':
            assert symbols['fw_image_identity']==APP_BASE+0x400
            assert symbols['__update_metadata_base__']==METADATA_BASE
            boot_symbols={}
            for line in subprocess.check_output([str(nm),'-n',str(d/'boot.elf')],text=True).splitlines():
                fields=line.split()
                if len(fields)==3:
                    try:boot_symbols[fields[2]]=int(fields[0],16)
                    except ValueError:pass
            assert boot_symbols['g_pfnVectors']==BOOT_BASE
            assert boot_symbols['fw_image_identity']==BOOT_BASE+0x400
            assert 'fw_update_handle' in boot_symbols and 'fw_update_flash_erase' in boot_symbols
            assert 'settings_store_save' not in boot_symbols,'Permanent updater must not save user settings'
            assert 'cpu_profile_command' not in boot_symbols,'Permanent updater must not include CPU profiling'
            assert 'adc_irq_probe_command' not in boot_symbols,'Permanent updater must not include ADC IRQ probing'
            assert 'adc_scan_satellite_service' not in boot_symbols,'Permanent updater must not process satellite Hall samples'
            assert ('usb_device_init' in boot_symbols)==(role=='left')
        assert 0x24000000<=symbols['__dma_start__']<=symbols['__dma_end__']<=0x24050000
        assert symbols['__dma_start__']%32==0 and symbols['__dma_end__']%32==0
        assert 'board_input_scan' in symbols and 'USART3_IRQHandler' in symbols and 'DMA2_Stream0_IRQHandler' in symbols
        if scan_hz==96000 and not a.adc_dual96_12bit and not a.adc_async96:
            assert info['adc_resolution_bits']==14 and info['adc_instances']==[1,2]
            assert info['adc_effective_clock_hz']==20000000 and info['adc_sample_cycles']==32.5
            assert symbols['DMA2_Stream1_IRQHandler']!=symbols['Default_Handler']
        if a.adc_dual96_12bit:
            assert info['adc_resolution_bits']==12 and info['adc_instances']==[1,2] and info['adc_ranks']==[4,3]
            assert info['adc_effective_clock_hz']==30000000 and info['adc_sample_cycles']==64.5
            assert info['dma_frames_per_irq']==2 and info['publication_target_hz']==48000
            assert info['filter_samples']==8 and info['filter_execution']=='per-batch IRQ running sum over both scans'
            assert info['sample_frame_bytes']==48 and info['hall_decision_precision']=='legacy integer ADC'
            assert symbols['DMA2_Stream1_IRQHandler']!=symbols['Default_Handler']
        if a.adc_dual64:
            assert info['adc_resolution_bits']==12 and info['adc_instances']==[1,2] and info['adc_ranks']==[4,3]
            assert info['adc_effective_clock_hz']==30000000 and info['adc_sample_cycles']==64.5
            assert info['dma_frames_per_irq']==1 and info['publication_target_hz']==64000
            assert info['filter_samples']==8 and info['filter_execution']=='per-scan IRQ running sum'
            assert symbols['DMA2_Stream1_IRQHandler']!=symbols['Default_Handler']
        if a.adc_async64 or a.adc_async96:
            assert info['adc_resolution_bits']==12 and info['adc_instances']==[1,2] and info['adc_ranks']==[4,3]
            assert info['adc_effective_clock_hz']==40000000 and info['adc_sample_cycles']==64.5
            assert info['dma_scans_per_bank']==32 and info['dma_frames_per_irq']==16
            assert info['dma_irq_target_hz_per_bank']==scan_hz//16 and info['publication_target_hz'] is None
            assert info['publication_mode']=='foreground latest completed per bank'
            assert info['filter_samples']==8 and info['filter_execution']=='foreground per-ADC eight-sample average'
            assert info['sample_frame_bytes']==48 and info['hall_decision_precision']=='legacy integer ADC'
            assert info['adc_sequence_meaning']=='minimum completed scan cursor across both ADC banks'
            assert symbols['DMA2_Stream1_IRQHandler']!=symbols['Default_Handler']
        if scan_hz==75000:
            assert info['adc_resolution_bits']==12 and info['adc_instances']==[1] and info['adc_ranks']==[7]
            assert info['adc_effective_clock_hz']==40000000 and info['adc_sample_cycles']==64.5
            assert info['dma_frames_per_irq']==1 and info['publication_target_hz']==75000
            assert info['filter_samples']==8 and info['filter_execution']=='per-scan IRQ running sum'
            assert symbols['DMA2_Stream1_IRQHandler']==symbols['Default_Handler']
        if a.adc_capture_diag:
            assert info['adc_resolution_bits']==12 and info['adc_effective_clock_hz']==40000000 and info['adc_sample_cycles']==64.5
            assert info['adc_instances']==([1,2] if a.adc_dual64_matched40 else [1])
            assert info['adc_ranks']==([4,3] if a.adc_dual64_matched40 else [7])
            assert info['dma_frames_per_irq']==1 and info['publication_target_hz']==64000 and info['filter_samples']==8
            assert info['filter_execution']=='per-scan IRQ running sum' and info['sample_frame_bytes']==48
            assert info['hall_decision_precision']=='legacy integer ADC'
            assert (symbols['DMA2_Stream1_IRQHandler']!=symbols['Default_Handler'])==a.adc_dual64_matched40
        if role=='left':assert 'usb_device_init' in symbols and 'settings_store_save' in symbols
        else:assert 'usb_device_init' not in symbols and 'settings_store_save' not in symbols
        assert ('board_cc_service' in symbols)==(role=='left' and a.hardware in ('c85','c86'))
        assert ('I2C1_EV_IRQHandler' in symbols and symbols['I2C1_EV_IRQHandler']!=symbols['Default_Handler'])==(role=='left' and a.hardware in ('c85','c86'))
        result[role]={'version':info['version'],'version_number':info['version_number'],'app_bytes':len(app),'factory_bytes':len(factory),'dma_bytes':symbols['__dma_end__']-symbols['__dma_start__'],'vector':'0x08020000','settings_preserved':True,'hardware_test':False,'cpu_profile':cpu_profile,'adc_irq_probe':a.adc_isr_probe,'adc_async64':a.adc_async64,'adc_async96':a.adc_async96,'source_tree_sha256':info['source_tree_sha256'],'files':info['files']}
        if a.hardware=='c86':result[role].update({'permanent_boot_bytes':len(boot),'metadata_address':hex(METADATA_BASE),'update_bundle_verified':True,'scan_target_hz':scan_hz,'adc_resolution_bits':info['adc_resolution_bits'],'adc_dual64':info['adc_dual64'],'adc_dual96_12bit':a.adc_dual96_12bit,'adc_capture_diag':a.adc_capture_diag,'adc_dual64_matched40':a.adc_dual64_matched40})
    (base/'image-verification.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
if __name__=='__main__':main()
