# SPDX-License-Identifier: GPL-2.0-or-later
"""Fetch verified upstream QEMU, apply the source overlay, and build ARM TCG."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PureWindowsPath
import subprocess
import sys
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
VERSION = '10.2.4'
SHA256 = '821b545b92f165e57dddccac5077d76d4d436a226595b8813ad59306bbfd0746'
SOURCES = ['qemu/nikon-z7-lab.c','qemu/nikon-z7-shoulder.h','qemu/nikon-z7-gpu.h','qemu/nikon-z7-isp.h','qemu/nikon-z7-isp-pixels.h','qemu/prepare_source.py']

def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        while chunk := f.read(1024*1024): h.update(chunk)
    return h.hexdigest()

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work-dir',type=Path,default=ROOT/'.build')
    p.add_argument('--archive',type=Path,help='Optional pre-downloaded original upstream tarball')
    p.add_argument('--jobs',type=int,default=4)
    p.add_argument('--msys-root',type=Path,help='Native Windows MSYS2 root (UCRT64 dependencies required)')
    a = p.parse_args()
    windows = os.name == 'nt'
    if windows and not a.msys_root:
        p.error('Use Linux/WSL or supply --msys-root for a native Windows build')
    environment = dict(os.environ)
    if windows:
        msys = a.msys_root.resolve(strict=True)
        environment.update(MSYSTEM='UCRT64', CHERE_INVOKING='yes')
        environment['PATH'] = os.pathsep.join([str(msys/'ucrt64/bin'), str(msys/'usr/bin'), environment['PATH']])

    def run(command, cwd=None):
        if windows:
            # The shell fragment is fixed; paths/options remain separate arguments.
            executable = Path(command[0])
            if executable.is_absolute():
                path = PureWindowsPath(executable)
                command = ['/' + path.drive[0].lower() + '/' + '/'.join(path.parts[1:]), *command[1:]]
            command = [str(msys/'usr/bin/bash.exe'), '-lc', 'exec "$@"', 'wonderlantern-build', *command]
        subprocess.run(command,cwd=cwd,env=environment,check=True)
    if os.environ.get('WSL_DISTRO_NAME') == 'ubuntu-noble':
        p.error('This coursework distribution is excluded; use ubuntu-noble-nikon')
    if not 1 <= a.jobs <= 64: p.error('--jobs must be 1..64')
    work = a.work_dir.resolve()
    work.mkdir(parents=True,exist_ok=True)
    archive = a.archive.resolve() if a.archive else work/f'qemu-{VERSION}.tar.xz'
    if not archive.exists():
        if a.archive: p.error('Requested archive does not exist')
        print('Downloading original upstream QEMU',flush=True)
        urllib.request.urlretrieve(f'https://download.qemu.org/qemu-{VERSION}.tar.xz',archive)
    if digest(archive) != SHA256: raise RuntimeError('Upstream QEMU archive SHA-256 mismatch')
    source = work/f'qemu-{VERSION}'
    marker = source/'.wonderlantern-extracted'
    if not marker.exists() or marker.read_text().strip() != SHA256:
        def source_filter(member, destination):
            # Upstream EDK2 contains host-header links for a separate ROM build.
            # They are unnecessary for this ARM emulator; never extract links
            # pointing outside the verified source archive's destination.
            try:
                return tarfile.data_filter(member, destination)
            except (tarfile.AbsoluteLinkError, tarfile.LinkOutsideDestinationError):
                return None
        with tarfile.open(archive,'r:xz') as t: t.extractall(work,filter=source_filter)
        marker.write_text(SHA256+'\n')
    subprocess.run([sys.executable,str(ROOT/'qemu/prepare_source.py'),str(source)],check=True)
    build = work/'build'
    build.mkdir(exist_ok=True)
    options = ['--target-list=arm-softmmu','--without-default-devices','--disable-docs','--disable-tools','--disable-user','--disable-werror','--disable-gtk','--disable-sdl','--disable-vnc','--disable-opengl','--disable-guest-agent','--disable-slirp','--disable-rust','--disable-debug-info']
    if not (build/'build.ninja').exists(): run([str(source/'configure'),*options],cwd=build)
    target = 'qemu-system-arm.exe' if windows else 'qemu-system-arm'
    run(['ninja','-j',str(a.jobs),target],cwd=build)
    binary = build/target
    manifest = dict(qemu_version=VERSION,upstream_archive_sha256=SHA256,binary=str(binary),binary_sha256=digest(binary),sources={name:digest(ROOT/name) for name in SOURCES},configure=options)
    manifest['generated_core_sources'] = {name:digest(source/name) for name in ['hw/intc/arm_gic.c','target/arm/helper.c','target/arm/tcg/op_helper.c']}
    if windows:
        manifest['native_runtime_path'] = str(msys/'ucrt64/bin')
    destination = ROOT/'.local/build-manifest.json'
    destination.parent.mkdir(exist_ok=True)
    destination.write_text(json.dumps(manifest,indent=2),encoding='utf-8')
    subprocess.run([str(binary),'--version'],env=environment,check=True)
    print('Build manifest: '+str(destination),flush=True)

if __name__ == '__main__': main()
