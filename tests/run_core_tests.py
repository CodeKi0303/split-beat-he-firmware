"""Host C tests for the actual core and flash journal, no device access."""
from pathlib import Path
import os,shutil,subprocess,sys
ROOT=Path(__file__).resolve().parents[1]
if hasattr(sys.stdout,'reconfigure'):sys.stdout.reconfigure(encoding='utf-8')
def compile_target(name,sources,defines=()):
    build=ROOT/'build/host';build.mkdir(parents=True,exist_ok=True)
    exe=build/(name+('.exe' if os.name=='nt' else ''))
    cc=next((shutil.which(x) for x in ['clang','gcc','cl'] if shutil.which(x)),None)
    vc=Path(r'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat')
    if not cc and os.name=='nt' and vc.exists():cc='cl'
    if not cc:raise RuntimeError('Install a host C compiler (clang/gcc/MSVC)')
    if Path(cc).stem=='cl':
        cmd=[cc,'/nologo','/std:c11','/W4','/WX','/O2',*(f'/D{x}' for x in defines),f'/I{ROOT/"include"}',f'/I{ROOT/"tests"}',*map(str,sources),f'/Fe:{exe}']
        if not shutil.which(cc):cmd='call "'+str(vc)+'" >nul && '+subprocess.list2cmdline(cmd)
    else:cmd=[cc,'-std=c11','-Wall','-Wextra','-Werror','-O2',*(f'-D{x}' for x in defines),'-I',str(ROOT/'include'),'-I',str(ROOT/'tests'),*map(str,sources),'-o',str(exe)]
    subprocess.run(cmd,cwd=build,shell=isinstance(cmd,str),check=True)
    return exe
def main():
    core=[ROOT/'src'/s for s in ['config.c','rapid_trigger.c','engine.c','protocol.c']]
    for name,sources in [('test_core',core+[ROOT/'tests/test_core.c']),('test_settings',[ROOT/'src/config.c',ROOT/'tests/test_settings.c'])]:
        exe=compile_target(name,sources);subprocess.run([str(exe)],check=True)
    for revision in (85,86):
        exe=compile_target(f'test_core_c{revision}',core+[ROOT/'tests/test_core.c'],[f'HARDWARE_REVISION={revision}'])
        subprocess.run([str(exe)],check=True)
    exe=compile_target('test_core_c86_scan96k',core+[ROOT/'tests/test_core.c'],
                       ['HARDWARE_REVISION=86','ROLE_LEFT=1','C86_SCAN_TARGET_HZ=96000U'])
    subprocess.run([str(exe)],check=True)
    exe=compile_target('test_async_engine',core+[ROOT/'tests/test_async_engine.c'],
                       ['HARDWARE_REVISION=86','ROLE_LEFT=1','C86_ADC_DUAL64=1','C86_ADC_ASYNC64=1'])
    subprocess.run([str(exe)],check=True)
    compile_target('core_bridge',core+[ROOT/'tests/core_bridge.c'])
if __name__=='__main__':main()
