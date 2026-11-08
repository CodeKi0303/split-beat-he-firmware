"""Content binding for offline firmware images; never accesses a device."""
from pathlib import Path
import hashlib
import json

ROOT = Path(__file__).resolve().parents[1]


def source_evidence():
    paths = [ROOT / 'build.py', Path(__file__).resolve(),
             ROOT / 'tools/firmware_artifacts.py']
    for tree in ('src', 'include', 'linker', 'third_party', 'hardware'):
        paths.extend(p for p in (ROOT / tree).rglob('*')
                     if p.is_file() and '__pycache__' not in p.parts and p.suffix != '.pyc')
    files = {p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
             for p in sorted(set(paths))}
    digest = hashlib.sha256(json.dumps(files, sort_keys=True).encode('utf-8')).hexdigest()
    return {'source_tree_sha256': digest, 'source_files_sha256': files}


def verify_source(info):
    current = source_evidence()
    if current != {key: info.get(key) for key in current}:
        raise RuntimeError('Build inputs changed or source binding missing; rebuild before verification/package')
