# SPDX-License-Identifier: Apache-2.0
# Scratch-only VS MSBuild probe and project/filter edit-plan experiment.
import hashlib
import base64
import ctypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import uuid


def main():
    host, msbuild_dir, fixture, transaction = map(Path, sys.argv[1:5])
    with tempfile.TemporaryDirectory(prefix="frost å 😀 ") as temporary:
        root = Path(temporary) / "tiny"
        shutil.copytree(fixture, root)
        project = root / "App" / "App.vcxproj"
        data = project.read_bytes()
        marker = b'<ClCompile Include="main.cpp" />'
        assert data.count(marker) == 1
        data = data.replace(marker, marker + b'\n    <ClCompile Include="extra.cpp" />')
        prefix, suffix = data.rsplit(b'</Project>', 1)
        project.write_bytes(prefix + b'<Import Project="scratch.props" /></Project>' + suffix)
        imported = project.parent / "scratch.props"
        imported.write_bytes(b'<Project><PropertyGroup><ScratchValue>one</ScratchValue></PropertyGroup></Project>')
        (project.parent / "extra.cpp").write_bytes(b'int extra() { return 1; }\n')
        filters = Path(str(project) + ".filters")
        filters.write_bytes(b'<?xml version="1.0" encoding="utf-8"?>\r\n'
                            b'<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">\r\n'
                            b'  <!-- keep this comment -->\r\n  <ItemGroup>\r\n'
                            b'    <ClCompile Include="extra.cpp"><Filter>Source Files</Filter></ClCompile>\r\n'
                            b'  </ItemGroup>\r\n</Project>\r\n')
        before_project, before_filters = project.read_bytes(), filters.read_bytes()
        request = {"Project": str(project), "Configuration": "Debug", "Platform": "x64", "Remove": "extra.cpp",
                   "Trusted": "true", "Root": str(root)}
        response = subprocess.run([str(host), str(msbuild_dir)], input=json.dumps(request, ensure_ascii=False) + "\n",
                                  text=True, encoding="utf-8", capture_output=True, check=True)
        result = json.loads(response.stdout)
        assert not result["Error"], result
        inline = (b'<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">'
                  b'<ItemGroup><ClCompile Include="extra.cpp"><Filter>Source Files</Filter></ClCompile>'
                  b'</ItemGroup></Project>')
        filters.write_bytes(inline)
        response = subprocess.run([str(host), str(msbuild_dir)], input=json.dumps(request, ensure_ascii=False) + "\n",
                                  text=True, encoding="utf-8", capture_output=True, check=True)
        assert not json.loads(response.stdout)["Error"], response.stdout
        filters.write_bytes(before_filters)
        assert result["Edit"]["ImportedInputs"] and sorted(result["Edit"]["Configurations"]) == ["Debug|x64", "Release|x64"]
        deceptive = before_project.replace(b'<ClCompile Include="extra.cpp" />',
            b'<!-- <ClCompile Include="extra.cpp" /> --><ClCompile Include="extra.cpp" Exclude="unused.cpp" />')
        assert deceptive != before_project
        project.write_bytes(deceptive)
        response = subprocess.run([str(host), str(msbuild_dir)], input=json.dumps(request, ensure_ascii=False) + "\n",
                                  text=True, encoding="utf-8", capture_output=True, check=True)
        assert "Text span does not identify" in json.loads(response.stdout)["Error"]
        hidden = before_project.replace(b'<ProjectConfiguration Include="Release|x64">',
                                        b'<ProjectConfiguration Condition="\'$(Configuration)\' == \'Release\'" Include="Release|x64">')
        assert hidden != before_project
        project.write_bytes(hidden)
        response = subprocess.run([str(host), str(msbuild_dir)], input=json.dumps(request, ensure_ascii=False) + "\n",
                                  text=True, encoding="utf-8", capture_output=True, check=True)
        assert json.loads(response.stdout)["Error"]
        conditional = (b'<ItemGroup Condition="\'$(Configuration)\' == \'Release\'">'
                       b'<ClCompile Remove="extra.cpp" /></ItemGroup>')
        prefix, suffix = before_project.rsplit(b'</Project>', 1)
        project.write_bytes(prefix + conditional + b'</Project>' + suffix)
        response = subprocess.run([str(host), str(msbuild_dir)], input=json.dumps(request, ensure_ascii=False) + "\n",
                                  text=True, encoding="utf-8", capture_output=True, check=True)
        assert json.loads(response.stdout)["Error"]
        project.write_bytes(before_project)
        entries = []
        for path, before, plan in ((project, before_project, result["Edit"]["Project"]),
                                   (filters, before_filters, result["Edit"]["Filters"])):
            assert hashlib.sha256(before).hexdigest().upper() == plan["Sha256"]
            assert path.read_bytes() == before
            edited = before[:plan["Offset"]] + before[plan["Offset"] + plan["Length"]:]
            assert edited == before.replace(before[plan["Offset"]:plan["Offset"] + plan["Length"]], b"", 1)
            entries.append({"Path": str(path), "Before": base64.b64encode(before).decode(),
                            "After": base64.b64encode(edited).decode()})
        journal = root / ".frost-project-edit-journal"
        transaction_request = {"Root": str(root), "Journal": str(journal), "Trusted": True,
                               "Recover": False, "StopAfterFirst": False, "Entries": entries,
                               "TransactionId": str(uuid.uuid4()), "Environment": result["Edit"]["Environment"],
                               "ImportedInputs": result["Edit"]["ImportedInputs"]}

        def transact(command, crash=False):
            process = subprocess.run([str(transaction)], input=json.dumps(command, ensure_ascii=False) + "\n", text=True, encoding="utf-8",
                                     capture_output=True, check=not crash)
            if crash:
                assert process.returncode == 86, (process.returncode, process.stdout, process.stderr)
                return "interrupted"
            return json.loads(process.stdout)["Error"]

        assert transact(dict(transaction_request, Trusted=False))
        assert "environment" in transact(dict(transaction_request, Environment="changed"))
        imported_before = imported.read_bytes()
        imported.write_bytes(imported_before.replace(b"one", b"two"))
        assert "Imported input changed" in transact(transaction_request)
        assert project.read_bytes() == before_project and filters.read_bytes() == before_filters
        imported.write_bytes(imported_before)
        escaped = [dict(entry) for entry in entries]
        escaped[0]["Path"] = str(root.parent / "outside.vcxproj")
        escaped[1]["Path"] = escaped[0]["Path"] + ".filters"
        assert transact(dict(transaction_request, Entries=escaped))
        assert not journal.exists()
        journal.write_bytes(b"existing journal")
        assert transact(transaction_request)
        assert journal.read_bytes() == b"existing journal"
        journal.unlink()
        hardlink = root / "hardlink.vcxproj"
        os.link(project, hardlink)
        try:
            assert transact(transaction_request)
            assert not journal.exists() and project.read_bytes() == before_project
        finally:
            hardlink.unlink()
        large_project = root / "large.vcxproj"
        large_filters = root / "large.vcxproj.filters"
        large = b"x" * (4 * 1024 * 1024)
        large_project.write_bytes(large)
        large_filters.write_bytes(large)
        encoded = base64.b64encode(large).decode()
        oversized = [dict(Path=str(path), Before=encoded, After=encoded)
                     for path in (large_project, large_filters)]
        assert "Journal exceeds" in transact(dict(transaction_request, Entries=oversized))
        assert not journal.exists() and large_project.read_bytes() == large and large_filters.read_bytes() == large
        case_dir = root / "case-sensitive"
        case_dir.mkdir()
        case_setup = subprocess.run(["fsutil.exe", "file", "setCaseSensitiveInfo", str(case_dir), "enable"],
                                    capture_output=True, text=True)
        if case_setup.returncode == 0:
            case_project = case_dir / "case.vcxproj"
            case_filters = case_dir / "case.vcxproj.filters"
            case_project.write_bytes(b"a")
            case_filters.write_bytes(b"b")
            case_entries = [dict(Path=str(path), Before=base64.b64encode(path.read_bytes()).decode(),
                                 After=base64.b64encode(path.read_bytes()).decode())
                            for path in (case_project, case_filters)]
            assert transact(dict(transaction_request, Entries=case_entries))
            assert not journal.exists()
            print("case-sensitive directory refusal passed")
        else:
            print("case-sensitive directory fixture unavailable:", case_setup.stdout.strip(), case_setup.stderr.strip())
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateFileW.restype = ctypes.c_void_p
        kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p,
                                      ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        lock = kernel.CreateFileW(str(project), 0x40000000, 1, None, 3, 0x80, None)
        assert lock != ctypes.c_void_p(-1).value
        try:
            assert transact(transaction_request)
            assert not journal.exists()
            assert project.read_bytes() == before_project and filters.read_bytes() == before_filters
        finally:
            kernel.CloseHandle(lock)
        transaction_request["StopAfterFirst"] = True
        assert transact(transaction_request, crash=True)
        assert journal.exists() and project.read_bytes() != before_project and filters.read_bytes() == before_filters
        original_journal = journal.read_bytes()
        journal_digest = hashlib.sha256(original_journal).hexdigest().upper()
        recovery = {"Root": str(root), "Journal": str(journal), "Trusted": True, "Recover": True,
                    "TransactionId": transaction_request["TransactionId"], "ExpectedProject": str(project),
                    "JournalDigest": journal_digest}
        tampered = json.loads(original_journal)
        tampered["Entries"][0]["Before"] = base64.b64encode(b"attacker supplied definition").decode()
        journal.write_text(json.dumps(tampered), encoding="utf-8")
        assert "private recovery record" in transact(recovery)
        assert project.read_bytes() != before_project and filters.read_bytes() == before_filters
        journal.write_bytes(original_journal)
        assert "Journal does not match" in transact(dict(recovery, TransactionId=str(uuid.uuid4())))
        original_object = root / "held-original.vcxproj"
        project.rename(original_object)
        project.write_bytes(original_object.read_bytes())
        assert "identity changed" in transact(recovery)
        project.unlink()
        original_object.rename(project)
        journal_link = root / "journal-hardlink"
        os.link(journal, journal_link)
        assert transact(recovery)
        assert journal.exists() and journal_link.exists()
        journal_link.unlink()
        filters.write_bytes(b"unexpected external content")
        assert transact(recovery)
        assert filters.read_bytes() == b"unexpected external content" and journal.exists()
        filters.write_bytes(before_filters)
        host_recovery = {"Root": str(root), "Project": str(project), "Trusted": "true", "Recover": "true",
                         "TransactionId": transaction_request["TransactionId"], "JournalDigest": journal_digest}
        recovery_decision = {"Accept": "true", "TransactionId": transaction_request["TransactionId"]}
        response = subprocess.run([str(host), str(msbuild_dir)],
                                  input=json.dumps(host_recovery, ensure_ascii=False) + "\n" + json.dumps(recovery_decision) + "\n",
                                  text=True, encoding="utf-8", capture_output=True, check=True)
        assert not json.loads(response.stdout.splitlines()[-1])["Error"], response.stdout
        assert project.read_bytes() == before_project and filters.read_bytes() == before_filters
        assert not journal.exists()
        transaction_request["StopAfterFirst"] = False
        assert not transact(transaction_request)
        assert not journal.exists()
        project.write_bytes(before_project)
        filters.write_bytes(before_filters)
        host_commit = dict(request, Commit="true", Trusted="false", Root=str(root), TransactionId=str(uuid.uuid4()))
        response = subprocess.run([str(host), str(msbuild_dir)], input=json.dumps(host_commit, ensure_ascii=False) + "\n",
                                  text=True, encoding="utf-8", capture_output=True, check=True)
        assert json.loads(response.stdout)["Error"]
        assert project.read_bytes() == before_project and filters.read_bytes() == before_filters
        host_commit["Trusted"] = "true"
        rejected = {"Accept": "false", "TransactionId": host_commit["TransactionId"]}
        response = subprocess.run([str(host), str(msbuild_dir)], input=json.dumps(host_commit, ensure_ascii=False) + "\n" + json.dumps(rejected) + "\n",
                                  text=True, encoding="utf-8", capture_output=True, check=True)
        assert json.loads(response.stdout.splitlines()[-1])["Error"]
        assert project.read_bytes() == before_project and filters.read_bytes() == before_filters
        wrong_id = {"Accept": "true", "TransactionId": str(uuid.uuid4())}
        response = subprocess.run([str(host), str(msbuild_dir)], input=json.dumps(host_commit, ensure_ascii=False) + "\n" + json.dumps(wrong_id) + "\n",
                                  text=True, encoding="utf-8", capture_output=True, check=True)
        assert json.loads(response.stdout.splitlines()[-1])["Error"]
        assert project.read_bytes() == before_project and filters.read_bytes() == before_filters
        decision = {"Accept": "true", "TransactionId": host_commit["TransactionId"]}
        response = subprocess.run([str(host), str(msbuild_dir)], input=json.dumps(host_commit, ensure_ascii=False) + "\n" + json.dumps(decision) + "\n",
                                  text=True, encoding="utf-8", capture_output=True, check=True)
        ready, completed = response.stdout.splitlines()
        assert json.loads(ready)["TransactionId"] == host_commit["TransactionId"]
        committed = json.loads(completed)
        assert not committed["Error"], committed
        assert not journal.exists()
        assert b"<!-- keep this comment -->" in filters.read_bytes()
        request.pop("Remove")
        response = subprocess.run([str(host), str(msbuild_dir)], input=json.dumps(request, ensure_ascii=False) + "\n",
                                  text=True, encoding="utf-8", capture_output=True, check=True)
        result = json.loads(response.stdout)
        assert not result["Error"], result
        assert not any(item["Type"] == "ClCompile" and item["EvaluatedInclude"] == "extra.cpp"
                       for item in result["Items"])
        if "--no-build" not in sys.argv[5:]:
            for solution in ("Tiny.sln", "Tiny.slnx"):
                subprocess.run([str(msbuild_dir / "MSBuild.exe"), str(root / solution), "-m:1",
                                "-p:Configuration=Debug", "-p:Platform=x64", "-nologo", "-v:minimal"], check=True)
        assert (project.parent / "extra.cpp").is_file()
        print("VS ownership, project/filter plan and reload passed" +
              ("; scratch build skipped" if "--no-build" in sys.argv[5:] else "; scratch build passed"))


if __name__ == "__main__":
    main()
