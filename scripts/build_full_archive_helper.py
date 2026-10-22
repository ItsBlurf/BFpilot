"""Build the separately linked embedded archive service with the PS5 SDK.
Windows compilation uses the workspace PS5 wrappers. No host compiler is used
for target code. No helper is launched or installed by this build script.
"""
import argparse,hashlib,json,os,shutil,subprocess
from pathlib import Path
parser=argparse.ArgumentParser()
parser.add_argument('--out',type=Path,default=Path('build/internal/bfpilot-full-archive.elf'))
parser.add_argument('--jobs',type=int,default=4)
args=parser.parse_args()
root=Path(__file__).resolve().parents[1]
bundle=root/'third_party/wfm-7zip-helper/CPP/7zip/Bundles/WfmHelper'
sdk=Path(os.environ['PS5_PAYLOAD_SDK'])
if os.name=='nt':
 cc,cxx='ps5-clang.cmd','ps5-c++.cmd'
 for compiler in [cc,cxx]:assert shutil.which(compiler),f'{compiler} unavailable; load ps5dev-env.ps1'
 strip=shutil.which('llvm-strip') or shutil.which('llvm-strip.exe')
 assert strip,'LLVM strip unavailable'
else:
 cc,cxx,strip=[str(sdk/'bin'/name) for name in ['prospero-clang','prospero-clang++','prospero-strip']]
command=['make','-f','makefile.ps5','-j'+str(max(1,min(args.jobs,8))),
         'CC='+cc,'CXX='+cxx,'STRIP='+strip]
subprocess.run(command,cwd=bundle,check=True)
destination=args.out if args.out.is_absolute() else root/args.out
destination.parent.mkdir(parents=True,exist_ok=True)
shutil.copy2(bundle/'wfm-7zip-helper.elf',destination)
print(json.dumps({'helper':str(destination),'sha256':hashlib.sha256(destination.read_bytes()).hexdigest(),'bytes':destination.stat().st_size}))
