# SPDX-License-Identifier: Apache-2.0
# Start independent updater instances with an intentionally stale lower candidate.
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

binary = str(Path(sys.argv[1]).resolve(strict=True))
with tempfile.TemporaryDirectory(prefix='frost-update-state-') as scratch:
    directory = Path(scratch)
    env = os.environ.copy()
    env['FROST_UPDATE_STATE_CHILD_DIR'] = str(directory)
    env['FROST_UPDATE_STATE_CHILD_MODE'] = 'lower'
    lower = subprocess.Popen([binary, '--gtest_filter=UpdateState.IndependentProcessChild'],
                             env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        deadline = time.monotonic() + 10
        while not (directory / 'lower-ready').exists():
            if lower.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError('Lower instance did not prepare its stale candidate')
            time.sleep(0.01)
        env['FROST_UPDATE_STATE_CHILD_MODE'] = 'higher'
        higher = subprocess.run([binary, '--gtest_filter=UpdateState.IndependentProcessChild'],
                                env=env, capture_output=True, text=True, timeout=10)
        print(higher.stdout, higher.stderr)
        if higher.returncode:
            raise RuntimeError('Higher instance failed to commit')
        (directory / 'continue-lower').write_text('continue', encoding='ascii')
        output, _ = lower.communicate(timeout=10)
        print(output)
        if lower.returncode:
            raise RuntimeError('Stale lower instance did not refuse publication')
        print('Independent instances: higher floor preserved; stale lower candidate refused')
    finally:
        if lower.poll() is None:
            lower.kill()
        lower.wait(timeout=10)
