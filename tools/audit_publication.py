# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the explicitly reviewed source inventory and every reachable Git blob.

This is a guard against unintended additions, not a license/copyright oracle.
--refresh-manifest is for maintainers after manual content/provenance review.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = 'publication-manifest.json'
FILES = {
    '.gitattributes','.gitignore','.github/workflows/ci.yml',
    'COPYING','LICENSES/QEMU-LICENSE.txt','NOTICE.md','CONTRIBUTING.md',
    'README.md','README.zh-CN.md','requirements.txt',
    'docs/build.md','docs/firmware.md','docs/usage.md','docs/architecture.md',
    'docs/debugging.md','docs/research-notes.md','docs/testing.md','docs/publication.md',
    'qemu/nikon-z7-lab.c','qemu/nikon-z7-gpu.h','qemu/nikon-z7-isp.h',
    'qemu/nikon-z7-isp-pixels.h','qemu/prepare_source.py',
    'tools/build.py','tools/cmos_config.py','tools/cmos_source.py',
    'tools/photo_cmos.py','tools/make_cmos.py','tools/raw_input.py','tools/rsp.py',
    'tools/probe.py','tools/capture_diagnostics.py','tools/interactive.py',
    'tools/interactive.html','tools/verify_ui.py','tools/smoke_qemu.py','tools/audit_publication.py',
    'tests/test_cmos_source.py','tests/test_raw_input.py','tests/test_gpu.c',
    'tests/test_isp.c','tests/include/ui/console.h','tests/run.sh',
    'qemu/nikon-z7-shoulder.h','tools/shoulder_protocol.py','tools/shoulder_display.py',
    'tests/test_shoulder_protocol.py','tests/test_shoulder_display.py','tests/test_shoulder_observer.c',
    'requirements-shoulder.txt','docs/shoulder-display.md',
    'tests/test_smoke_qemu.py',
}
IGNORED = {'.git','.local','.build','.venv','__pycache__'}
SECRET_PATTERNS = [
    re.compile(r'gh[opusr]_[A-Za-z0-9]{30,}'),
    re.compile(r'github_pat_[A-Za-z0-9_]{30,}'),
    re.compile(r'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----'),
]

def validate(path, data):
    if path not in FILES | {MANIFEST}: raise ValueError('Unreviewed public path: '+path)
    if len(data)>512*1024 or b'\0' in data: raise ValueError('Unexpected binary/large content: '+path)
    text = data.decode('utf-8')
    if any(pattern.search(text) for pattern in SECRET_PATTERNS): raise ValueError('Credential pattern in '+path)
    if re.search(r"['\"][0-9a-fA-F]{512,}['\"]",text): raise ValueError('Large embedded hex data needs review: '+path)
    if path.endswith(('.py','.c','.h','.sh')) and 'SPDX-License-Identifier: GPL-2.0-or-later' not in text[:200]:
        raise ValueError('Missing source license: '+path)
    return hashlib.sha256(data).hexdigest()

def git(*args):
    return subprocess.check_output(['git',*args],cwd=ROOT)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--refresh-manifest',action='store_true')
    a = p.parse_args()
    discovered = set()
    for directory,dirs,files in os.walk(ROOT):
        dirs[:] = [name for name in dirs if name not in IGNORED]
        for name in files:
            path = Path(directory)/name
            # Linked worktrees use a .git metadata file instead of a directory.
            if path == ROOT/'.git' and not path.is_symlink(): continue
            if path.is_symlink(): raise ValueError('Symlink in public tree: '+str(path))
            discovered.add(path.relative_to(ROOT).as_posix())
    if discovered - {MANIFEST} != FILES:
        raise ValueError('Inventory differs: '+str({'extra':sorted(discovered-FILES-{MANIFEST}),'missing':sorted(FILES-discovered)}))
    hashes = {name:validate(name,(ROOT/name).read_bytes()) for name in sorted(FILES)}
    if a.refresh_manifest:
        (ROOT/MANIFEST).write_text(json.dumps({'schema':1,'scope':'reviewed source-only publication inventory; no runtime artifacts','files':hashes},indent=2)+'\n',encoding='utf-8',newline='\n')
    manifest = json.loads((ROOT/MANIFEST).read_text(encoding='utf-8'))
    if manifest.get('schema') != 1 or manifest.get('files') != hashes: raise ValueError('Content changed since reviewed manifest')
    validate(MANIFEST,(ROOT/MANIFEST).read_bytes())
    commits = 0; blobs = set()
    if (ROOT/'.git').exists():
        # Inspect all refs, rather than only the currently visible checkout.
        refs = git('rev-list','--all').decode().splitlines()
        commits = len(refs)
        for ref in refs:
            tree = git('ls-tree','-rz','--full-tree',ref).split(b'\0')
            for row in tree:
                if not row: continue
                meta,name = row.split(b'\t',1)
                mode,kind,oid = meta.decode().split()
                name = name.decode('utf-8')
                if kind != 'blob' or mode not in ('100644','100755'): raise ValueError('Unexpected history object: '+name)
                if (oid,name) not in blobs:
                    validate(name,git('cat-file','blob',oid)); blobs.add((oid,name))
        tracked = set(git('ls-files','-z').decode().strip('\0').split('\0'))
        if tracked != FILES | {MANIFEST}: raise ValueError('Git index differs from reviewed inventory')
    print(json.dumps({'passed':True,'reviewed_files':len(hashes)+1,'history_commits':commits,'history_blobs':len(blobs),'runtime_data_excluded':True}))

if __name__ == '__main__': main()
