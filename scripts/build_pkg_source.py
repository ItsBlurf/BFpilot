"""Build the embedded source service with the native SDK wrapper only."""
import argparse, hashlib, json, os, shutil, subprocess
from pathlib import Path
root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--out', type=Path, default=Path('build/internal/bf-pkg-source.elf'))
p.add_argument('--library', type=Path, default=Path('build/internal/libsmb2.a'))
p.add_argument('--build', type=Path, default=Path('build/pkg-source'))
a = p.parse_args()
sdk = Path(os.environ['PS5_PAYLOAD_SDK'])
cc = str(sdk/'win/prospero-clang.cmd') if os.name == 'nt' else str(sdk/'bin/prospero-clang')
assert shutil.which(cc), 'load ps5dev-env.ps1 first'
build = a.build if a.build.is_absolute() else root/a.build
output = a.out if a.out.is_absolute() else root/a.out
library = a.library if a.library.is_absolute() else root/a.library
assert library.is_file()
build.mkdir(parents=True, exist_ok=True); output.parent.mkdir(parents=True, exist_ok=True)
names = ['pkg_source_main', 'pkg_source_store', 'pkg_source_codec', 'pkg_source_wire_io',
         'pkg_source_http', 'pkg_source_stream', 'pkg_file', 'remote_io', 'smb', 'path_ops', 'fs_at']
objects = []
for name in names:
    obj = build/(name+'.o'); relative = Path(os.path.relpath(obj, root)).as_posix(); objects.append(relative)
    subprocess.run([cc, '-Os', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                    '-ffunction-sections', '-fdata-sections', '-Isrc', '-Ithird_party/libsmb2/include',
                    '-c', 'src/'+name+'.c', '-o', relative], cwd=root, check=True)
# The SDK's Windows compiler-driver linker forwarding requires paths relative
# to the project when the checkout's absolute path contains spaces.
if os.name == 'nt':
    # The SDK batch driver can add startup objects twice with newer Clang.
    # Link once, explicitly, as the main payload does on Windows.
    subprocess.run([str(sdk/'win/prospero-lld.exe'), '-m', 'elf_x86_64_fbsd',
                    '--sysroot='+str(sdk), '-pie', '--eh-frame-hdr', '--hash-style=sysv',
                    '--build-id=sha1', '--unresolved-symbols=report-all', '-z', 'now',
                    '-z', 'start-stop-visibility=hidden', '-z', 'rodynamic',
                    '-z', 'common-page-size=0x4000', '-z', 'max-page-size=0x4000',
                    '--default-script', 'main.script', '--gc-sections',
                    '-L'+str(sdk/'target/lib'), '-L'+str(sdk/'target/user/homebrew/lib'),
                    '-l:crt1.o', '-l:crti.o', '-l:crtbegin.o', '-o', Path(os.path.relpath(output,root)).as_posix(),
                    *objects, Path(os.path.relpath(library,root)).as_posix(), '-lpthread', '-lc', '-lkernel_web',
                    '-lSceLibcInternal', '-lSceNet', '-lc_stub_weak', '-lkernel_stub_weak',
                    '-l:crtend.o', '-l:crtn.o'], cwd=root, check=True)
else:
    subprocess.run([cc, '-Wl,--gc-sections', '-o', Path(os.path.relpath(output, root)).as_posix(),
                    *objects, Path(os.path.relpath(library, root)).as_posix(), '-lpthread'], cwd=root, check=True)
strip = shutil.which('llvm-strip') if os.name == 'nt' else str(sdk/'bin/prospero-strip')
subprocess.run([strip, '--strip-all', str(output)], check=True)
print(json.dumps({'service':str(output),'sha256':hashlib.sha256(output.read_bytes()).hexdigest(),'bytes':output.stat().st_size}))
