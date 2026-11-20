# SPDX-License-Identifier: Apache-2.0
# Payload admission refuses changed source inputs, assemblies and unlisted sidecars without tools.
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import roslyn_host
class RoslynPayload(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "root"
        (self.root / "sdk").mkdir(parents=True)
        (self.root / "sdk/sdk.lock.json").write_text(json.dumps({"windows": {"dotnet": {"version": "10.0.403"}}}))
        (self.root / "sdk/managed-dependencies.json").write_text("{}")
        source = self.root / "integrations/roslyn-host"
        source.mkdir(parents=True)
        (source / "Program.cs").write_text("// owned source\n")
        self.payload = Path(self.temporary.name) / "payload"
        self.payload.mkdir()
        (self.payload / roslyn_host.ENTRY).write_bytes(b"receipt fixture; never executed")
        record = {"schemaVersion": 1, "sourceInputs": roslyn_host.source_inputs(self.root),
                  "dependencyManifest": roslyn_host.sha(self.root / "sdk/managed-dependencies.json"),
                  "sdkVersion": "10.0.403", "files": {roslyn_host.ENTRY: roslyn_host.sha(self.payload / roslyn_host.ENTRY)}}
        (self.payload / roslyn_host.RECORD).write_text(json.dumps(record))
    def test_exact_payload_admits_and_changed_assembly_refuses(self):
        roslyn_host.verify_payload(self.payload, self.root)
        (self.payload / roslyn_host.ENTRY).write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "file changed"):
            roslyn_host.verify_payload(self.payload, self.root)
    def test_unlisted_sidecar_and_changed_source_refuse(self):
        (self.payload / "unreviewed.dll").write_bytes(b"foreign")
        with self.assertRaisesRegex(ValueError, "file list"):
            roslyn_host.verify_payload(self.payload, self.root)
        (self.payload / "unreviewed.dll").unlink()
        (self.root / "integrations/roslyn-host/Program.cs").write_text("// edited source\n")
        with self.assertRaisesRegex(ValueError, "source/dependency"):
            roslyn_host.verify_payload(self.payload, self.root)
if __name__ == "__main__":
    unittest.main()
