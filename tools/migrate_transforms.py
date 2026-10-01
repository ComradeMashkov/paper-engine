#!/usr/bin/env python3
"""Explicit DCMO 2 -> 3 migration on a NEW project copy; originals are never written."""
import argparse
import difflib
import math
from pathlib import Path
import shutil
import tempfile
from native_data import read, dumps

LEGACY_VERSION, TARGET_VERSION = 2, 3
EXTENSIONS = {'.dcscene', '.dcworld', '.dctemplates', '.dcresources'}
HALF_ANGLE = 0.5
XYZ_COMPONENTS = 3

def migrate(source, destination):
    source, destination = Path(source).resolve(), Path(destination).resolve()
    if not source.is_dir() or destination.exists() or destination.is_relative_to(source):
        raise ValueError('Require a source directory and a new destination outside it')
    if any(p.is_symlink() for p in source.rglob('*')):
        raise ValueError('Project copy must not contain symbolic links')
    documents = []
    for path in sorted(source.rglob('*')):
        if path.suffix not in EXTENSIONS:
            continue
        data = read(path)
        if data.get('version') not in (LEGACY_VERSION, TARGET_VERSION):
            raise ValueError(f'Unsupported document version: {path}')
        data['version'] = TARGET_VERSION
        def convert(value):
            if isinstance(value, dict):
                if 'yaw' in value:
                    angle = value.pop('yaw') * HALF_ANGLE
                    value['rotation'] = [0.0, math.sin(angle), 0.0, math.cos(angle)]
                if isinstance(value.get('scale'), (int, float)):
                    value['scale'] = [value['scale']] * XYZ_COMPONENTS # numbers: XYZ components.
                for child in value.values():
                    convert(child)
            elif isinstance(value, list):
                for child in value:
                    convert(child)
        # Spawn camera yaw is intentionally retained: camera pitch/yaw is a separate contract.
        if path.suffix == '.dcscene':
            for node in data.get('scene', {}).get('nodes', []):
                convert(node)
        if path.suffix == '.dctemplates':
            convert(data.get('templates', {}))
        result = dumps(data)
        original = path.read_text()
        patch = ''.join(difflib.unified_diff(original.splitlines(True), result.splitlines(True),
                    fromfile=str(path.relative_to(source)), tofile=str(path.relative_to(source))))
        documents.append((path.relative_to(source), result, patch))
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.paper-migration-', dir=destination.parent) as staging:
        copy = Path(staging) / 'project'
        shutil.copytree(source, copy)
        for relative, result, _ in documents:
            (copy / relative).write_text(result)
        (copy / 'transforms-v3.patch').write_text(''.join(patch for _, _, patch in documents))
        if destination.exists():
            raise ValueError('Destination appeared during migration')
        copy.rename(destination)
    return len(documents)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    print(f'Migrated {migrate(args.source, args.destination)} documents on a project copy')
