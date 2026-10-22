#!/usr/bin/env python3
"""
build_near.py - builds with Android NDK:
  hassle_near.c  -> libHassleExtended_arm64.so / libHassleExtended_arm32.so   (мод «игроки рядом» + HTTP)

  python build_near.py [path_to_NDK]
"""
import glob, os, platform, subprocess, sys

here = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(here, 'hassle_near.c')

def find_ndk():
    if len(sys.argv) > 1: return sys.argv[1]
    for k in ('ANDROID_NDK_HOME', 'ANDROID_NDK_ROOT', 'ANDROID_NDK'):
        if os.environ.get(k): return os.environ[k]
    sdks = [os.environ.get('ANDROID_HOME'), os.environ.get('ANDROID_SDK_ROOT'),
            os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Android', 'Sdk'),
            os.path.expanduser('~/Android/Sdk'), os.path.expanduser('~/Library/Android/sdk')]
    for s in sdks:
        if s:
            c = sorted(glob.glob(os.path.join(s, 'ndk', '*')))
            if c: return c[-1]
    sys.exit('NDK not found. Pass the path as an argument.')

ndk = find_ndk()
host = {'Windows': 'windows-x86_64', 'Linux': 'linux-x86_64', 'Darwin': 'darwin-x86_64'}[platform.system()]
clang = os.path.join(ndk, 'toolchains', 'llvm', 'prebuilt', host, 'bin', 'clang' + ('.exe' if os.name == 'nt' else ''))
if not os.path.exists(clang):
    sys.exit('clang not found: ' + clang)

targets = [
    ('arm64', 'aarch64-linux-android21', ['-Wl,-z,max-page-size=16384']),
    ('arm32', 'armv7a-linux-androideabi21', []),
]
for name, tgt, extra in targets:
    out = os.path.join(here, 'libHassleExtended_%s.so' % name)
    cmd = [clang, '--target=' + tgt, '-O2', '-shared', '-fPIC', '-fvisibility=hidden', '-fno-stack-protector',
           '-Wall', '-Wno-unused-function', '-Wl,-soname,libHassleExtended.so', '-Wl,--build-id=none', '-s',
           src, '-o', out] + extra
    print('>', name)
    r = subprocess.run(cmd)
    if r.returncode: sys.exit('build %s failed' % name)
    print('   ', out, os.path.getsize(out), 'bytes')
print('done')
