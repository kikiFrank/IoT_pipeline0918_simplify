#!/usr/bin/env python3
"""Remove development material from the fixed 165 delivery tree.

Default: inspect only. --apply directly deletes the selected material without
backup, then verifies that runtime executables, libraries and models are unchanged.
Runtime scripts, assets, configuration and dynamically loaded .cl kernels stay.
"""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import shutil
import tarfile
import zipfile

ROOT = Path('/mnt/workspace/0924')
SOURCE_SUFFIXES = {'.c', '.cc', '.cpp', '.cxx', '.c++', '.h', '.hh', '.hpp', '.hxx',
                   '.h++', '.ipp', '.inl', '.tpp', '.cu', '.cuh', '.go', '.rs',
                   '.java', '.kt', '.swift', '.proto'}
DOC_SUFFIXES = {'.md', '.markdown', '.mdown', '.mdx'}
BUILD_SUFFIXES = {'.o', '.obj', '.a', '.lib', '.cmake', '.pyc', '.pyo'}
DEV_DIRS = {'.git', 'CMakeFiles', '__pycache__'}
DEV_TREES = {Path('aibox_pipeline') / name for name in
             ('src', 'include', 'contracts', 'tests', 'docs')}
BUILD_NAMES = {'CMakeLists.txt', 'CMakeCache.txt', 'Makefile', 'makefile',
               'build.ninja', '.ninja_deps', '.ninja_log', 'compile_commands.json',
               'install_manifest.txt', '.gitignore', '.gitattributes', '.gitmodules'}


def checked(path):
    # Do not resolve a symlink leaf: deleting it must never touch its target.
    if not path.is_absolute() or path == ROOT or ROOT not in path.parents:
        raise RuntimeError(f'Unsafe target: {path}')
    path.parent.resolve(strict=True).relative_to(ROOT)
    return path


def elf_runtime(path):
    if path.is_symlink() or not path.is_file():
        return False
    with path.open('rb') as stream:
        header = stream.read(18)
    if len(header) < 18 or header[:4] != b'\x7fELF':
        return False
    order = 'little' if header[5] == 1 else 'big'
    return int.from_bytes(header[16:18], order) in (2, 3)


def archive_has_sources(path):
    if not path.name.endswith(('.tgz', '.tar', '.tar.gz', '.tar.xz', '.tar.bz2', '.zip')):
        return False
    def forbidden(name):
        entry = Path(name)
        return entry.suffix.lower() in SOURCE_SUFFIXES | DOC_SUFFIXES or '.git' in entry.parts
    if path.suffix == '.zip':
        with zipfile.ZipFile(path) as archive:
            return any(forbidden(name) for name in archive.namelist())
    with tarfile.open(path, 'r:*') as archive:
        return any(forbidden(entry.name) for entry in archive)


def candidates():
    result = []
    for directory, dirs, files in os.walk(ROOT, followlinks=False):
        base = Path(directory)
        for name in list(dirs):
            path = base / name
            relative = path.relative_to(ROOT)
            if name in DEV_DIRS or relative in DEV_TREES:
                result.append((checked(path), 'development-directory'))
                dirs.remove(name)
        for name in files:
            path = base / name
            suffix = path.suffix.lower()
            reason = None
            if suffix in SOURCE_SUFFIXES:
                reason = 'development-source'
            elif suffix in DOC_SUFFIXES:
                reason = 'markdown'
            elif suffix in BUILD_SUFFIXES or name in BUILD_NAMES:
                reason = 'build-intermediate'
            elif 'build_board' in path.parts or 'build_unified' in path.parts:
                if not elf_runtime(path) and '.so' not in path.suffixes:
                    reason = 'build-intermediate'
            elif not path.is_symlink() and archive_has_sources(path):
                reason = 'source-archive'
            if reason:
                result.append((checked(path), reason))
    return result


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def payload_hashes(selected):
    result = {}
    removed = {path for path, _ in selected}
    for directory, dirs, files in os.walk(ROOT, followlinks=False):
        base = Path(directory)
        dirs[:] = [name for name in dirs if base / name not in removed]
        for name in files:
            path = base / name
            if path in removed or path.is_symlink():
                continue
            if path.suffix == '.bin' or '.so' in path.suffixes or elf_runtime(path):
                result[str(path.relative_to(ROOT))] = digest(path)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    if ROOT.is_symlink() or ROOT.resolve(strict=True) != ROOT:
        raise RuntimeError('Delivery root must be the exact physical /mnt/workspace/0924')
    selected = candidates()
    print(json.dumps({'selected_entries': len(selected),
                      'reasons': dict(Counter(reason for _, reason in selected))}, sort_keys=True))
    for path, reason in selected:
        print(f'{reason}\t{path.relative_to(ROOT)}')
    if not args.apply:
        return
    hashes = payload_hashes(selected)
    deleted_files = 0
    for path, _ in selected:
        checked(path)
        if path.is_dir() and not path.is_symlink():
            deleted_files += sum(len(files) for _, _, files in os.walk(path, followlinks=False))
            shutil.rmtree(path)
        else:
            deleted_files += 1
            path.unlink()
    # Leave empty runtime upload/cache directories in place: they may be
    # intentionally provisioned even when they currently contain no files.
    if candidates():
        raise RuntimeError('Development files remain')
    for relative, expected in hashes.items():
        if digest(ROOT / relative) != expected:
            raise RuntimeError(f'Runtime payload changed: {relative}')
    report = {'status': 'DELIVERY_CLEAN_OK', 'deleted_files': deleted_files,
              'runtime_payloads_verified': len(hashes), 'backup_created': False,
              'remaining_source_or_md': 0}
    print(json.dumps(report, sort_keys=True))


if __name__ == '__main__':
    main()
