# SPDX-License-Identifier: Apache-2.0
# Records the exact arguments forwarded by the compiler-cache launcher.
import json
from pathlib import Path
import sys

Path(sys.argv[1]).write_text(json.dumps(sys.argv[2:]), encoding="utf-8")
