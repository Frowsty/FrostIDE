// SPDX-License-Identifier: Apache-2.0
// The Windows ProjectHost (net472): MSBuild evaluation for the Solution Explorer (ProjectEvaluator.cs,
// SLN-2), project-file edits through the MSBuild construction API (ProjectFileEdits.cs, SLN-6) and
// solution edits through SolutionPersistence (SolutionEdits.cs, SLN-6); an installed VS MSBuild
// supplies the assemblies.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text;
using System.Web.Script.Serialization;

static class VsProbe
{
    static void Main(string[] args)
    {
        Console.InputEncoding = new UTF8Encoding(false, true);
        Console.OutputEncoding = new UTF8Encoding(false, true);
        if ((args.Length != 1 && args.Length != 3) || !Path.IsPathRooted(args[0]))
            throw new ArgumentException("Pass the absolute VS MSBuild assembly directory");
        string directory = args[0];
        RegisterInstance(directory);
        // Microsoft.Build* live in the given directory (Bin\amd64); SolutionPersistence only in Bin,
        // the parent, as MSBuild.exe's own configuration resolves it.
        string parent = Path.GetDirectoryName(directory.TrimEnd('\\'));
        AppDomain.CurrentDomain.AssemblyResolve += (_, e) => {
            string name = new AssemblyName(e.Name).Name + ".dll";
            foreach (string candidate in new[] { Path.Combine(directory, name), parent == null ? null : Path.Combine(parent, name) })
                if (candidate != null && File.Exists(candidate)) return Assembly.LoadFrom(candidate);
            return null;
        };
        if (args.Length == 3)
        {
            if (args[1] != "--replay-binlog" || !Path.IsPathRooted(args[2]))
                throw new ArgumentException("Expected --replay-binlog and an absolute log path");
            Replay(args[2]);
            return;
        }
        Run(directory);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    static void Replay(string path) { FrostBuildLogger.Replay(path); }

    /* What MSBuildLocator's RegisterMSBuildPath does for a Visual Studio instance on .NET Framework
     * (microsoft/MSBuildLocator, MIT; ProjectEvaluator.cs explains why the host does it itself):
     * MSBUILD_EXE_PATH names the instance's MSBuild.exe, VSINSTALLDIR its root and
     * VisualStudioVersion its major version, so Microsoft.Build resolves that instance's toolset.
     * `directory` is <root>\MSBuild\Current\Bin\amd64 (or Bin). Nothing of Microsoft.Build is
     * loaded yet: FileVersionInfo reads the version resource only. */
    static void RegisterInstance(string directory)
    {
        string msbuild = Path.Combine(directory, "MSBuild.exe");
        string assembly = Path.Combine(directory, "Microsoft.Build.dll");
        if (!File.Exists(msbuild) || !File.Exists(assembly))
            throw new ArgumentException("The MSBuild directory has no MSBuild.exe or Microsoft.Build.dll");
        var bin = new DirectoryInfo(directory);
        if (!bin.Name.Equals("Bin", StringComparison.OrdinalIgnoreCase)) bin = bin.Parent;
        var msbuildRoot = bin?.Parent?.Parent;
        var root = msbuildRoot?.Parent;
        if (bin == null || !bin.Name.Equals("Bin", StringComparison.OrdinalIgnoreCase) || root == null ||
            !msbuildRoot.Name.Equals("MSBuild", StringComparison.OrdinalIgnoreCase))
            throw new ArgumentException("The MSBuild directory is not inside a Visual Studio instance");
        int major = FileVersionInfo.GetVersionInfo(assembly).FileMajorPart;
        Environment.SetEnvironmentVariable("MSBUILD_EXE_PATH", msbuild);
        Environment.SetEnvironmentVariable("VSINSTALLDIR", root.FullName.TrimEnd('\\') + "\\");
        Environment.SetEnvironmentVariable("VisualStudioVersion", major + ".0");
    }

    /*
     * Two protocols share stdin and stdout. Requests with an "Op" are the evaluation protocol
     * (ProjectEvaluator.cs, one answer line per request). Requests without one are the edit
     * protocol of src/project/include/frost/project/project_edit.hpp: one edit per helper process,
     * Recover="true" (replay a journal backwards), "Solution" (SolutionEdits.cs) or "Project"
     * (ProjectFileEdits.cs). A committing edit writes {"Ready":true,"TransactionId","JournalDigest"}
     * once its journal is durable and waits for {"Accept":"true","TransactionId"} before writing the
     * definitions; the last line is the answer, {"Error":null} or {"Error":"<reason>"}.
     */
    [MethodImpl(MethodImplOptions.NoInlining)]
    static void Run(string msbuildDirectory)
    {
        var json = new JavaScriptSerializer { MaxJsonLength = Int32.MaxValue };
        string line;
        while ((line = Console.ReadLine()) != null)
        {
            string op = null, opId = null;
            try
            {
                var request = json.Deserialize<Dictionary<string, string>>(line);
                if (request != null && request.TryGetValue("Op", out op))
                {
                    request.TryGetValue("Id", out opId);
                    if (op == "Hello")
                        Console.WriteLine(json.Serialize(ProjectEvaluator.Hello(msbuildDirectory)));
                    else if (op == "Evaluate")
                        Console.WriteLine(json.Serialize(ProjectEvaluator.Evaluate(request)));
                    else if (op == "ReplayBinlog")
                    {
                        string log;
                        if (!request.TryGetValue("Path", out log) || !Path.IsPathRooted(log))
                            throw new InvalidOperationException("Absolute binary log path required");
                        FrostBuildLogger.Replay(log);
                        Console.WriteLine(json.Serialize(new { Op = op, Id = opId, Error = (string)null }));
                    }
                    else if (op == "ClCommandLines")
                        Console.WriteLine(json.Serialize(ProjectEvaluator.ClCommandLines(request)));
                    else
                        throw new InvalidOperationException("Unknown operation " + op);
                    continue;
                }
                if (request == null) throw new InvalidOperationException("Empty request");
                string trustedRoot;
                if (!request.TryGetValue("Trusted", out trustedRoot) || trustedRoot != "true")
                    throw new InvalidOperationException("Trusted root required before evaluation");
                string rootPath;
                if (!request.TryGetValue("Root", out rootPath) || !Path.IsPathRooted(rootPath))
                    throw new InvalidOperationException("Trusted root required");
                string solution, path;
                request.TryGetValue("Solution", out solution);
                request.TryGetValue("Project", out path);
                Func<string, string, bool> approve = (id, digest) => AwaitApproval(json, id, digest);
                string recovery;
                if (request.TryGetValue("Recover", out recovery) && recovery == "true")
                {
                    if (path == null || !Path.IsPathRooted(path)) throw new ArgumentException("Project path must be absolute");
                    string recoveryId = request["TransactionId"];
                    TransactionProbe.Execute(new TransactionRequest {
                        Root = rootPath, Journal = Path.Combine(rootPath, ".frost-project-edit-journal"),
                        Trusted = true, Recover = true, TransactionId = recoveryId, ExpectedProject = path,
                        JournalDigest = request["JournalDigest"]
                    }, digest => AwaitApproval(json, recoveryId, digest));
                    Console.WriteLine(json.Serialize(new { Error = (string)null }));
                    continue;
                }
                if ((solution == null) == (path == null))
                    throw new InvalidOperationException("Name either a project or a solution to edit");
                if (!Path.IsPathRooted(solution ?? path)) throw new ArgumentException("Project path must be absolute");
                object answer = solution != null ? SolutionEdits.Handle(request, rootPath, solution, approve)
                                                 : ProjectFileEdits.Handle(request, rootPath, path, approve);
                Console.WriteLine(json.Serialize(answer));
            }
            catch (Exception e)
            {
                string message = e is AggregateException && e.InnerException != null ? e.InnerException.Message : e.Message;
                if (op != null)
                    Console.WriteLine(json.Serialize(new { Op = op, Id = opId, Error = message }));
                else
                    Console.WriteLine(json.Serialize(new { Items = (object)null, Error = message }));
            }
        }
    }

    static bool AwaitApproval(JavaScriptSerializer json, string id, string digest)
    {
        Console.WriteLine(json.Serialize(new { Ready = true, TransactionId = id, JournalDigest = digest }));
        string acknowledgement = Console.ReadLine();
        if (acknowledgement == null) throw new InvalidOperationException("Commit was not acknowledged");
        var decision = json.Deserialize<Dictionary<string, string>>(acknowledgement);
        string accepted, decisionId;
        if (decision == null || !decision.TryGetValue("Accept", out accepted) || accepted != "true" ||
            !decision.TryGetValue("TransactionId", out decisionId) || decisionId != id)
            return false;
        return true;
    }
}
