# SPDX-License-Identifier: Apache-2.0
# Offline upgrade admission and release evidence validation; this tool never runs a gate itself.
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

RELEASE_GATES = ('windows-full', 'linux-full', 'budgets', 'windows-sanitizers',
                 'linux-sanitizers', 'corpus-cpp', 'corpus-lua', 'corpus-frostengine',
                 'installer', 'accessibility', 'demo-pack', 'user-signoff')

def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def validate_lock(node, location='lock'):
    if isinstance(node, dict):
        if 'url' in node:
            if not isinstance(node['url'], str) or not node['url'].startswith('https://'):
                raise ValueError(f'{location}: artifacts require HTTPS URLs')
            if not (re.fullmatch('[0-9a-f]{64}', node.get('sha256', '')) or re.fullmatch('[0-9a-f]{128}', node.get('sha512', ''))):
                raise ValueError(f'{location}: artifact SHA-256 required')
        for name, child in node.items(): validate_lock(child, location + '.' + name)
    elif isinstance(node, list):
        for child in node: validate_lock(child, location)

def require_structure(baseline, candidate, location='lock'):
    if type(candidate) is not type(baseline):
        raise ValueError(f'{location}: pin type changed')
    if isinstance(baseline, dict):
        if location.endswith('.archiveSha256'):
            if not candidate or any(not re.fullmatch('[0-9a-f]{64}', item) for item in candidate.values()):
                raise ValueError(f'{location}: nonempty archive digest map required')
            return
        for key, value in baseline.items():
            if key not in candidate: raise ValueError(f'{location}.{key}: required pin removed')
            require_structure(value, candidate[key], location + '.' + key)
    elif isinstance(baseline, list):
        if not candidate or any(not isinstance(item, str) or not item.strip() for item in candidate):
            raise ValueError(f'{location}: nonempty archive list required')
    elif isinstance(candidate, str) and not candidate.strip():
        raise ValueError(f'{location}: empty pin value')

def upgrade_plan(current, candidate, adr):
    if not adr.is_file() or adr.suffix != '.md': raise ValueError('upgrade requires a decision ADR')
    old = json.loads(current.read_text(encoding='utf-8'))
    new = json.loads(candidate.read_text(encoding='utf-8'))
    if old.get('schemaVersion') != 1 or new.get('schemaVersion') != 1:
        raise ValueError('unsupported lock schema')
    require_structure(old, new)
    validate_lock(new)
    if old == new: raise ValueError('candidate does not change any pins')
    for platform in ('windows', 'linux'):
        if not isinstance(new.get(platform, {}).get('llvm'), dict) or not isinstance(new.get('qt'), dict):
            raise ValueError('candidate must retain both platform LLVM and Qt toolchains')
    return {'schema': 1, 'kind': 'upgrade-plan', 'currentSha256': sha256(current),
            'candidateSha256': sha256(candidate), 'adrSha256': sha256(adr),
            'requiredGates': list(RELEASE_GATES), 'status': 'runtime-verification-pending'}

def release_gate(manifest_path, commit, lock):
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    if not re.fullmatch('[0-9a-f]{40}', commit): raise ValueError('full source commit required')
    if manifest.get('schema') != 1 or manifest.get('commit') != commit or manifest.get('lockSha256') != sha256(lock):
        raise ValueError('evidence does not identify this source commit and toolchain lock')
    gates = manifest.get('gates', {})
    for name in RELEASE_GATES:
        entry = gates.get(name, {})
        if entry.get('status') != 'passed': raise ValueError(f'{name}: passing evidence required')
        # Local records are content-addressed and constrained to the manifest directory.
        relative = Path(entry.get('file', ''))
        path = (manifest_path.parent / relative).resolve()
        if relative.is_absolute() or not path.is_relative_to(manifest_path.parent.resolve()) or not path.is_file():
            raise ValueError(f'{name}: missing local evidence file')
        if entry.get('sha256') != sha256(path): raise ValueError(f'{name}: evidence hash mismatch')
        record = json.loads(path.read_text(encoding='utf-8'))
        if (record.get('gate') != name or record.get('commit') != commit or
                record.get('lockSha256') != manifest['lockSha256'] or record.get('status') != 'passed'):
            raise ValueError(f'{name}: evidence record is not bound to this gate/source/lock')
        if not isinstance(entry.get('reviewer'), str) or not entry['reviewer'].strip():
            raise ValueError(f'{name}: reviewer/signatory required')
    return {'schema': 1, 'kind': 'release-evidence-validation', 'commit': commit,
            'gates': list(RELEASE_GATES), 'status': 'recorded-evidence-valid'}

def main():
    parser = argparse.ArgumentParser(description='Offline local upgrade/release evidence checks; no builds or network')
    sub = parser.add_subparsers(dest='action', required=True)
    upgrade = sub.add_parser('upgrade-plan')
    upgrade.add_argument('--current', type=Path, required=True)
    upgrade.add_argument('--candidate', type=Path, required=True)
    upgrade.add_argument('--adr', type=Path, required=True)
    release = sub.add_parser('release-gate')
    release.add_argument('--manifest', type=Path, required=True)
    release.add_argument('--commit', required=True)
    release.add_argument('--lock', type=Path, required=True)
    args = parser.parse_args()
    result = (upgrade_plan(args.current, args.candidate, args.adr) if args.action == 'upgrade-plan'
              else release_gate(args.manifest, args.commit, args.lock))
    print(json.dumps(result, indent=2))

if __name__ == '__main__':
    try: main()
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f'delivery gate: {error}', file=sys.stderr)
        sys.exit(1)
