"""Import pinned sources from explicitly supplied local upstream checkouts.

The resulting third_party tree is committed, so normal builds need neither the
parent workspace nor network access. This script never accesses a device.
"""
from pathlib import Path
import argparse,hashlib,json,shutil,subprocess
ROOT=Path(__file__).resolve().parents[1]
PIN={"weact-h723":"0a08d2808291a7ff38c8a4f89748693019257345","stm32-usb-device":"0b06460a43c30670a764cb7e58c5a7918e789f6e","cmsis-device-h7":"6dac8c24d7b38ab20806d27dd7d8285a6433b8f7"}
def validate_checkouts(vendor):
    if not vendor.is_dir():
        raise ValueError(f'Vendor directory does not exist: {vendor}')
    for name,sha in PIN.items():
        checkout=(vendor/name).resolve()
        if not checkout.is_dir() or not checkout.is_relative_to(vendor):
            raise ValueError(f'Missing or out-of-tree checkout: {name}')
        def git(*args):
            return subprocess.check_output(['git','-C',str(checkout),*args],text=True,encoding='utf-8',stderr=subprocess.PIPE).strip()
        if Path(git('rev-parse','--show-toplevel')).resolve()!=checkout:
            raise ValueError(f'{name} must be its own Git checkout')
        if git('rev-parse','HEAD')!=sha:
            raise ValueError(f'{name} pin mismatch; expected {sha}')
        if git('status','--porcelain','--untracked-files=no'):
            raise ValueError(f'{name} has tracked local changes')

def import_plan(vendor):
    """Validate every selected source before any destination file is changed."""
    example=vendor/'weact-h723/Examples'
    plan=[]
    def copy(src,dst):
        source=src.resolve()
        if not source.is_file() or not source.is_relative_to(vendor):
            raise ValueError(f'Missing or out-of-tree source: {src}')
        plan.append((src,Path(dst)))
    hal=example/'07-ADC_Test/Drivers/STM32H7xx_HAL_Driver'
    for src in (hal/'Inc').rglob('*.h'):copy(src,Path('hal/Inc')/src.relative_to(hal/'Inc'))
    sdinc=example/'04-SD_Test/Drivers/STM32H7xx_HAL_Driver/Inc'
    for src in sdinc.rglob('*.h'):
        relative=src.relative_to(sdinc)
        if not (hal/'Inc'/relative).exists():copy(src,Path('hal/Inc')/relative)
    modules=['hal','hal_adc','hal_adc_ex','hal_cortex','hal_dma','hal_dma_ex','hal_flash','hal_flash_ex','hal_gpio','hal_pwr','hal_pwr_ex','hal_rcc','hal_rcc_ex','hal_tim','hal_tim_ex','hal_i2c','hal_i2c_ex']
    for item in modules:copy(hal/'Src'/f'stm32h7xx_{item}.c',Path('hal/Src')/f'stm32h7xx_{item}.c')
    for item in ['hal_pcd','hal_pcd_ex','ll_usb']:
        src=example/'04-SD_Test/Drivers/STM32H7xx_HAL_Driver/Src'/f'stm32h7xx_{item}.c';copy(src,Path('hal/Src')/src.name)
    copy(hal/'LICENSE.txt','hal/LICENSE.txt')
    core=example/'07-ADC_Test/Drivers/CMSIS'
    for src in (core/'Include').rglob('*.h'):copy(src,Path('cmsis/Include')/src.relative_to(core/'Include'))
    for filename in ['License.txt','LICENSE.txt','LICENSE']:
        if (core/filename).exists():
            copy(core/filename,Path('cmsis')/'License.txt')
            break # Windows aliases License.txt / LICENSE.txt; record only one.
    dev=vendor/'cmsis-device-h7'
    for name in ['stm32h7xx.h','stm32h723xx.h','system_stm32h7xx.h']:copy(dev/'Include'/name,Path('device/Include')/name)
    copy(dev/'Source/Templates/gcc/startup_stm32h723xx.s','device/startup_stm32h723xx.s')
    copy(example/'04-SD_Test/Core/Src/system_stm32h7xx.c','device/system_stm32h7xx.c')
    copy(dev/'LICENSE.md','device/LICENSE.md')
    usb=vendor/'stm32-usb-device'
    for src in (usb/'Core').rglob('*'):
        if src.suffix in ('.c','.h') and 'template' not in src.name:copy(src,Path('usb/Core')/src.relative_to(usb/'Core'))
    copy(usb/'LICENSE.md','usb/LICENSE.md')
    if not any(dst.parts[:2]==('cmsis','Include') for _,dst in plan):
        raise ValueError('CMSIS core headers are missing')
    if not any(dst==Path('cmsis/License.txt') for _,dst in plan):
        raise ValueError('CMSIS core license is missing')
    return plan

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vendor',required=True,type=Path,
                        help='Directory containing local weact-h723, cmsis-device-h7 and stm32-usb-device checkouts at the pinned revisions; no download is performed')
    parser.add_argument('--check-only',action='store_true',help='Validate checkouts and selected files without modifying third_party')
    args=parser.parse_args()
    vendor=args.vendor.expanduser().resolve()
    try:
        validate_checkouts(vendor)
        plan=import_plan(vendor)
    except (ValueError,subprocess.CalledProcessError) as error:
        parser.error(str(error))
    if args.check_only:
        print(f'Validated {len(plan)} source files; third_party was not changed')
        return
    out=ROOT/'third_party'
    copies=[]
    for src,relative in plan:
        dst=out/relative;dst.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(src,dst)
        copies.append({'path':dst.relative_to(ROOT).as_posix(),'source':src.relative_to(vendor).as_posix(),'sha256':hashlib.sha256(dst.read_bytes()).hexdigest()})
    (out/'manifest.json').write_text(json.dumps({'pins':PIN,'files':copies},indent=2)+'\n',encoding='utf-8')
    print(f'Imported {len(copies)} files with source hashes')
if __name__=='__main__':main()
