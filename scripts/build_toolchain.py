"""Record target build inputs so SDK/compiler changes invalidate cached objects."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def fingerprint(sdk=None, llvm_bindir=None):
    sdk = Path(sdk or os.environ['PS5_PAYLOAD_SDK']).resolve()
    bindir = llvm_bindir or os.environ.get('LLVM_BINDIR')
    compiler = Path(bindir) / ('clang.exe' if os.name == 'nt' else 'clang') if bindir else None
    if not compiler or not compiler.is_file():
        resolved = shutil.which('clang')
        if not resolved:
            raise RuntimeError('clang not found; set LLVM_BINDIR or PATH')
        compiler = Path(resolved)
    version = subprocess.check_output([str(compiler), '--version'], text=True).strip()
    paths = [sdk/'toolchain/prospero.mk', sdk/'target/include/ps5/kernel.h',
             sdk/'target/include/ps5/payload.h', sdk/'target/include/sys/syscall.h']
    paths += sorted((sdk/'target/lib').glob('*.o'))
    paths += sorted((sdk/'target/lib').glob('*.a'))
    paths += sorted((sdk/'target/lib').glob('*.so'))
    paths += sorted((sdk/'ldscripts').glob('*'))
    linker = sdk/'win/prospero-lld.exe' if os.name == 'nt' else sdk/'bin/prospero-lld'
    paths.append(linker)
    for path in paths:
        if not path.is_file():
            raise RuntimeError('missing SDK build input: ' + str(path))
    return dict(sdk=str(sdk), compiler=str(compiler.resolve()), version=version,
                inputs={p.relative_to(sdk).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                        for p in paths})


def ensure_stamp(path, sdk=None, llvm_bindir=None):
    value = fingerprint(sdk, llvm_bindir)
    data = json.dumps(value, indent=2, sort_keys=True) + '\n'
    path = Path(path)
    if not path.exists() or path.read_text(encoding='utf-8') != data:
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_suffix('.tmp')
        temporary.write_text(data, encoding='utf-8', newline='\n')
        temporary.replace(path)
    return hashlib.sha256(data.encode()).hexdigest()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--stamp', type=Path, required=True)
    parser.add_argument('--sdk')
    parser.add_argument('--llvm-bindir')
    args = parser.parse_args()
    print(ensure_stamp(args.stamp, args.sdk, args.llvm_bindir))
