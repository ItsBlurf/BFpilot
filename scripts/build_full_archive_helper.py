"""Build the separately linked embedded archive service with the PS5 SDK.
Windows compilation uses the SDK's PS5 wrappers. No host compiler is used
for target code. No helper is launched or installed by this build script.
"""
import argparse,hashlib,json,os,shutil,subprocess
from pathlib import Path
from build_toolchain import ensure_stamp
parser=argparse.ArgumentParser()
parser.add_argument('--out',type=Path,default=Path('build/internal/bfpilot-full-archive.elf'))
parser.add_argument('--jobs',type=int,default=4)
args=parser.parse_args()
root=Path(__file__).resolve().parents[1]
bundle=root/'third_party/wfm-7zip-helper/CPP/7zip/Bundles/WfmHelper'
sdk=Path(os.environ['PS5_PAYLOAD_SDK'])
toolchain_id=ensure_stamp(root/'build/archive-toolchain.json')
if os.name=='nt':
 cc,cxx=[str(sdk/'win'/name) for name in ['prospero-clang.cmd','prospero-clang++.cmd']]
 for compiler in [cc,cxx]:assert shutil.which(compiler),f'{compiler} unavailable; load ps5dev-env.ps1'
 strip=shutil.which('llvm-strip') or shutil.which('llvm-strip.exe')
 assert strip,'LLVM strip unavailable'
else:
 cc,cxx,strip=[str(sdk/'bin'/name) for name in ['prospero-clang','prospero-clang++','prospero-strip']]
command=['make','-f','makefile.ps5','-j'+str(max(1,min(args.jobs,8))),
         'BUILD_DIR=_o_ps5/t'+toolchain_id[:8],
         'CC="'+Path(cc).as_posix()+'"','CXX="'+Path(cxx).as_posix()+'"','STRIP='+strip]
if os.name=='nt':
 # Keep the object list in a response file through the final Windows linker.
 # Clang can otherwise expand it into a command longer than cmd.exe accepts.
 extra=root/'build/archive-objects.mk'
 extra.parent.mkdir(parents=True,exist_ok=True)
 extra.write_text('.PHONY: bfpilot-objects\nbfpilot-objects: $(OBJECTS)\n\t$(file >$(BUILD_DIR)/bfpilot-objects.rsp,$(OBJECTS))\n',encoding='utf-8',newline='\n')
 command+=['-f',str(extra),'bfpilot-objects']
subprocess.run(command,cwd=bundle,check=True)
if os.name=='nt':
 libraries=sdk/'target/lib'
 prefix=['-m','elf_x86_64_fbsd','--sysroot='+str(sdk),'-pie','--eh-frame-hdr',
         '--hash-style=sysv','--build-id=sha1','--unresolved-symbols=report-all',
         '-z','now','-z','start-stop-visibility=hidden','-z','rodynamic',
         '-z','common-page-size=0x4000','-z','max-page-size=0x4000',
         '--default-script','main.script','--gc-sections','-L'+str(libraries),
         '-L'+str(sdk/'target/user/homebrew/lib'),'-L.',
         '-l:crt1.o','-l:crti.o','-l:crtbegin.o']
 subprocess.run([str(sdk/'win/prospero-lld.exe'),'-o','wfm-7zip-helper.elf',*prefix,
                 '@_o_ps5/t'+toolchain_id[:8]+'/bfpilot-objects.rsp','-lSceNotification','-lpthread','-lc++',
                 '-lc++abi','-lunwind','-lc','-lkernel_web','-lSceLibcInternal',
                 '-lSceNet','-lc_stub_weak','-lkernel_stub_weak','-l:crtend.o',
                 '-l:crtn.o'],cwd=bundle,check=True)
 subprocess.run([strip,'--strip-all','wfm-7zip-helper.elf'],cwd=bundle,check=True)
destination=args.out if args.out.is_absolute() else root/args.out
destination.parent.mkdir(parents=True,exist_ok=True)
shutil.copy2(bundle/'wfm-7zip-helper.elf',destination)
print(json.dumps({'helper':str(destination),'sha256':hashlib.sha256(destination.read_bytes()).hexdigest(),'bytes':destination.stat().st_size}))
