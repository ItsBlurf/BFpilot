"""PS5 libsmb2 build; pinned source and complete relink inputs stay bundled."""
import concurrent.futures,hashlib,json,os,re,shutil,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[1];source=root/'third_party/libsmb2'
build=root/'build/smb2';build.mkdir(parents=True,exist_ok=True)
assert os.environ.get('PS5_PAYLOAD_SDK'),'load the PS5 environment first'
assert shutil.which('ps5-clang.cmd') if os.name=='nt' else True
compiler='ps5-clang.cmd' if os.name=='nt' else str(Path(os.environ['PS5_PAYLOAD_SDK'])/'bin/prospero-clang')
names=list(dict.fromkeys(re.findall(r'\b([a-zA-Z0-9_-]+\.c)\b',(source/'lib/Makefile.am').read_text())))
names=[name for name in names if name!='krb5-wrapper.c']
headers=list((source/'include').rglob('*.h'))+list((source/'lib').glob('*.h'))+[source/'ps5/config.h',Path(__file__)]
latest=max(p.stat().st_mtime_ns for p in headers)
def compile_one(name):
 obj=build/(name[:-2]+'.o');path=source/'lib'/name
 if obj.exists() and obj.stat().st_mtime_ns>=max(latest,path.stat().st_mtime_ns):return
 command=[compiler,'-O2','-fPIC','-ffunction-sections','-fdata-sections','-DHAVE_CONFIG_H','-D_U_=__attribute__((unused))','-D_FILE_OFFSET_BITS=64','-Ips5','-Iinclude','-Iinclude/smb2','-Ilib','-c','lib/'+name,'-o',Path(os.path.relpath(obj,source)).as_posix()]
 process=subprocess.run(command,cwd=source,capture_output=True,text=True)
 if process.returncode:raise RuntimeError(name+'\n'+process.stdout+process.stderr)
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:list(pool.map(compile_one,names))
archive=root/'build/internal/libsmb2.a';archive.parent.mkdir(parents=True,exist_ok=True)
ar=shutil.which('llvm-ar') if os.name=='nt' else str(Path(os.environ['PS5_PAYLOAD_SDK'])/'bin/prospero-ar')
subprocess.run([ar,'rcs',str(archive),*[str(build/(name[:-2]+'.o')) for name in names]],check=True)
print(json.dumps({'library':'libsmb2','version':'6.1.0','sha256':hashlib.sha256(archive.read_bytes()).hexdigest()}))
