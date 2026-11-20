#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Explicit offline dialect source builder and Luau registry-manifest producer; never called on project open.
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
from licensecheck import ALLOWLIST
ROOT = Path(__file__).resolve().parents[1]

def digest(path: Path) -> str:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def pin(name: str) -> dict:
    record = json.loads((ROOT / 'sdk/lua-dialects/sources.json').read_text(encoding='utf-8'))['sources'][name]
    if record['license'] not in ALLOWLIST: raise ValueError('Dialect source license is not allowed')
    return record

def registry_manifest(executable: Path, license_file: Path) -> dict:
    record = pin('luau-lsp')
    for file in (executable, license_file):
        if not file.is_absolute() or not file.is_file() or file.is_symlink():
            raise ValueError('Luau tools/license must be explicit regular absolute paths')
    if digest(license_file) != record['licenseSha256']:
        raise ValueError('Luau LSP license differs from the pinned source notice')
    return {'version': 1, 'servers': [{'id':'luau', 'name':'Luau LSP', 'languageId':'luau',
        'version':record['version'], 'executable':str(executable), 'enabled':True, 'extensions':['.luau'],
        'arguments':['lsp'], 'license':'MIT', 'licensePath':str(license_file), 'licenseSha256':record['licenseSha256'],
        'artifacts':[{'path':str(executable), 'sha256':digest(executable)}],
        'configuration':{'luau-lsp':{'platform':{'type':'standard'}}}, 'initializationOptions':{}}]}

def build(args) -> None:
    record = pin(args.kind)
    source, work = args.source, args.work
    if not source.is_absolute() or not source.is_dir() or not work.is_absolute() or work.exists():
        raise ValueError('Select an existing source checkout and a fresh absolute owned build directory')
    if source.resolve().is_relative_to(work.resolve()) or work.resolve().is_relative_to(source.resolve()):
        raise ValueError('Source and build trees must be separate')
    for name in ('git', 'clang', 'cmake', 'ninja'):
        tool = getattr(args, name)
        if not tool.is_absolute() or not tool.is_file(): raise ValueError('Explicit absolute tool required: '+name)
    if not args.clang.name.lower().startswith('clang'): raise ValueError('Dialect builds require explicit Clang')
    if any(path.is_symlink() for path in source.rglob('*')): raise ValueError('Source recipe does not follow source symlinks')
    def git(*values):
        return subprocess.run([str(args.git), '-C', str(source), *values], check=True, capture_output=True, text=True).stdout.strip()
    if git('rev-parse', 'HEAD') != record['commit'] or git('status', '--porcelain', '--untracked-files=all'):
        raise ValueError('Dialect source checkout must be clean at the exact immutable source pin')
    submodules = subprocess.run([str(args.git), '-C', str(source), 'submodule', 'status', '--recursive'], check=True, capture_output=True, text=True).stdout
    if any(line and line[0] in '-+U' for line in submodules.splitlines()):
        raise ValueError('All source-pinned submodules must already exist at the recorded gitlink; no downloads are attempted')
    if digest(source / record['licensePath']) != record['licenseSha256']:
        raise ValueError('Dialect source license receipt mismatch')
    work.mkdir(parents=True, exist_ok=False)
    copied = work / 'source'
    shutil.copytree(source, copied, ignore=shutil.ignore_patterns('.git'))
    environment = os.environ.copy()
    if args.kind == 'luajit':
        if os.name == 'nt':
            # Patch only the private copy, with exact known upstream substitutions. All tools are explicit.
            script = copied / 'src/msvcbuild.bat'
            text = script.read_text(encoding='utf-8')
            replacements = {'@set LJCOMPILE=cl ': '@set LJCOMPILE="'+str(args.clang)+'" ',
                            '@set LJLINK=link ': '@set LJLINK="'+str(args.clang.parent / 'lld-link.exe')+'" ',
                            '@set LJLIB=lib ': '@set LJLIB="'+str(args.clang.parent / 'llvm-lib.exe')+'" '}
            for before, after in replacements.items():
                if text.count(before) != 1: raise ValueError('Pinned LuaJIT build recipe changed')
                text = text.replace(before, after)
            script.write_text(text, encoding='utf-8')
            cmd = Path(environment['SystemRoot']) / 'System32/cmd.exe'
            subprocess.run([str(cmd), '/c', str(script), 'static'], cwd=script.parent, env=environment, check=True)
            artifact = copied / 'src/luajit.exe'
        else:
            if not args.make or not args.make.is_absolute() or not args.make.is_file(): raise ValueError('Explicit absolute make required')
            subprocess.run([str(args.make), 'CC='+str(args.clang), 'HOST_CC='+str(args.clang)], cwd=copied, env=environment, check=True)
            artifact = copied / 'src/luajit'
    else:
        output = work / 'build'
        subprocess.run([str(args.cmake), '-S', str(copied), '-B', str(output), '-G', 'Ninja',
            '-DCMAKE_MAKE_PROGRAM='+str(args.ninja), '-DCMAKE_CXX_COMPILER='+str(args.clang), '-DCMAKE_BUILD_TYPE=Release',
            '-DFETCHCONTENT_FULLY_DISCONNECTED=ON', '-DFETCHCONTENT_UPDATES_DISCONNECTED=ON',
            '-DLUAU_BUILD_TESTS=OFF', '-DLUAU_BUILD_WEB=OFF'], check=True)
        target = 'Luau.Repl.CLI' if args.kind == 'luau' else 'luau-lsp'
        subprocess.run([str(args.cmake), '--build', str(output), '--target', target], check=True)
        artifact = output / (('luau' if args.kind == 'luau' else 'luau-lsp') + ('.exe' if os.name == 'nt' else ''))
    receipt = {'version':1, 'source':record, 'artifact':str(artifact), 'sha256':digest(artifact),
               'recipeSha256':digest(Path(__file__)), 'runtimeExecuted':False, 'testsExecuted':False}
    (work / 'frost-dialect-build.json').write_text(json.dumps(receipt, indent=2)+'\n', encoding='utf-8')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    manifest = commands.add_parser('manifest'); manifest.add_argument('--executable', type=Path, required=True); manifest.add_argument('--license-file', type=Path, required=True)
    builder = commands.add_parser('build'); builder.add_argument('--kind', choices=('luau','luau-lsp','luajit'), required=True)
    for name in ('source','work','git','clang','cmake','ninja'): builder.add_argument('--'+name, type=Path, required=True)
    builder.add_argument('--make', type=Path)
    args = parser.parse_args()
    if args.command == 'manifest': print(json.dumps(registry_manifest(args.executable, args.license_file), indent=2))
    else: build(args)
if __name__ == '__main__': main()
