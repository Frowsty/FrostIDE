// SPDX-License-Identifier: Apache-2.0
// Experimental Windows-only, recoverable scratch project-file transaction.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Web.Script.Serialization;
using Microsoft.Win32.SafeHandles;

sealed class TransactionRequest
{
    public string Root { get; set; }
    public string Journal { get; set; }
    public bool Trusted { get; set; }
    public bool Recover { get; set; }
    public bool StopAfterFirst { get; set; }
    public TransactionEntry[] Entries { get; set; }
    public string TransactionId { get; set; }
    public string Environment { get; set; }
    public Dictionary<string, string> ImportedInputs { get; set; }
    public string ExpectedProject { get; set; }
    public string JournalDigest { get; set; }
}

sealed class TransactionEntry
{
    public string Path { get; set; }
    public string Before { get; set; }
    public string After { get; set; }
    public uint Volume { get; set; }
    public uint IndexHigh { get; set; }
    public uint IndexLow { get; set; }
}

sealed class Journal
{
    public int Version { get; set; }
    public string Root { get; set; }
    public TransactionEntry[] Entries { get; set; }
    public string TransactionId { get; set; }
}

static class TransactionProbe
{
    static readonly JavaScriptSerializer Json = new JavaScriptSerializer { MaxJsonLength = 64 * 1024 * 1024 };
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern SafeFileHandle CreateFileW(string path, uint access, uint share, IntPtr security,
                                             uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetFileInformationByHandle(SafeFileHandle handle, out FileInformation info);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetFileInformationByHandleEx(SafeFileHandle handle, int kind, out uint info, uint size);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetFileInformationByHandle(SafeFileHandle handle, int kind, ref byte info, uint size);
    [StructLayout(LayoutKind.Sequential)]
    struct FileInformation
    {
        public uint Attributes, CreationLow, CreationHigh, AccessLow, AccessHigh, WriteLow, WriteHigh;
        public uint Volume, SizeHigh, SizeLow, Links, IndexHigh, IndexLow;
    }

    static SafeFileHandle OpenChecked(string path, bool directory, bool create, bool journal, bool readOnly = false)
    {
        const uint genericRead = 0x80000000, genericWrite = 0x40000000, deleteAccess = 0x00010000, shareRead = 1;
        const uint openExisting = 3, createNew = 1, openReparse = 0x00200000, backup = 0x02000000;
        var handle = CreateFileW(path, genericRead | (directory || readOnly ? 0 : genericWrite) | (journal ? deleteAccess : 0), journal ? 0 : shareRead,
                                 IntPtr.Zero, create ? createNew : openExisting,
                                 openReparse | (directory ? backup : 0), IntPtr.Zero);
        if (handle.IsInvalid)
        {
            int error = Marshal.GetLastWin32Error();
            handle.Dispose();
            throw new IOException("Cannot exclusively open " + path + " (Win32 " + error + ")");
        }
        FileInformation info;
        if (!GetFileInformationByHandle(handle, out info) || (info.Attributes & 0x400) != 0 ||
            ((info.Attributes & 0x10) != 0) != directory || (!directory && !readOnly && info.Links != 1))
        {
            handle.Dispose();
            throw new IOException("Unexpected file type or reparse point: " + path);
        }
        uint caseFlags;
        if (directory && (!GetFileInformationByHandleEx(handle, 23, out caseFlags, sizeof(uint)) || (caseFlags & 1) != 0))
        {
            handle.Dispose();
            throw new IOException("Case-sensitive or unsupported directory: " + path);
        }
        return handle;
    }

    static void DeleteHeldJournal(FileStream journal)
    {
        byte delete = 1;
        if (!SetFileInformationByHandle(journal.SafeFileHandle, 4, ref delete, 1))
            throw new IOException("Cannot mark the held journal for deletion (Win32 " + Marshal.GetLastWin32Error() + ")");
    }

    static void HoldDirectories(string path, List<SafeFileHandle> handles, HashSet<string> held)
    {
        string full = Path.GetFullPath(path);
        string current = Path.GetPathRoot(full);
        if (held.Add(current)) handles.Add(OpenChecked(current, true, false, false));
        foreach (string component in full.Substring(current.Length).Split(new[] { Path.DirectorySeparatorChar },
                                                                        StringSplitOptions.RemoveEmptyEntries))
        {
            current = Path.Combine(current, component);
            if (held.Add(current)) handles.Add(OpenChecked(current, true, false, false));
        }
    }

    sealed class EvaluationFence : IDisposable
    {
        public readonly List<SafeFileHandle> Handles = new List<SafeFileHandle>();
        public readonly List<FileStream> Streams = new List<FileStream>();
        public void Dispose() {
            foreach (var stream in Streams) stream.Dispose(); Streams.Clear();
            foreach (var handle in Handles) handle.Dispose(); Handles.Clear();
        }
    }

    internal static IDisposable FenceEvaluation(string root, string path)
    {
        if (!Path.IsPathRooted(root) || !Path.IsPathRooted(path))
            throw new InvalidOperationException("Trusted root and project must be absolute");
        var fence = new EvaluationFence();
        try
        {
            var held = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            HoldDirectories(root, fence.Handles, held);
            string checkedPath = CheckedPath(root, path, true);
            HoldDirectories(Path.GetDirectoryName(checkedPath), fence.Handles, held);
            fence.Handles.Add(OpenChecked(checkedPath, false, false, false, true));
            return fence;
        }
        catch { fence.Dispose(); throw; }
    }

    internal static IDisposable FenceImports(IDictionary<string, string> inputs)
    {
        var fence = new EvaluationFence();
        try
        {
            var held = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var input in inputs)
            {
                if (!Path.IsPathRooted(input.Key)) throw new InvalidOperationException("Import path must be absolute");
                HoldDirectories(Path.GetDirectoryName(input.Key), fence.Handles, held);
                var handle = OpenChecked(input.Key, false, false, false, true);
                var stream = new FileStream(handle, FileAccess.Read);
                fence.Streams.Add(stream);
                using (var sha = SHA256.Create())
                {
                    string actual = BitConverter.ToString(sha.ComputeHash(ReadAll(stream))).Replace("-", "");
                    if (!String.Equals(actual, input.Value, StringComparison.OrdinalIgnoreCase))
                        throw new InvalidOperationException("Imported input changed: " + input.Key);
                }
            }
            return fence;
        }
        catch { fence.Dispose(); throw; }
    }

    static void Main()
    {
        Console.InputEncoding = new UTF8Encoding(false, true);
        Console.OutputEncoding = new UTF8Encoding(false, true);
        string line;
        while ((line = Console.ReadLine()) != null)
        {
            try
            {
                var request = Json.Deserialize<TransactionRequest>(line);
                Execute(request);
                Console.WriteLine(Json.Serialize(new { Error = (string)null }));
            }
            catch (Exception e)
            {
                Console.WriteLine(Json.Serialize(new { Error = e.Message }));
            }
        }
    }

    // Reject links along the whole path: a lexical containment check alone is not a trust boundary.
    // This is a bounded scratch prototype, not protection against privileged path replacement races.
    static string CheckedPath(string root, string path, bool exists)
    {
        string fullRoot = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar);
        string full = Path.GetFullPath(path);
        if (!Path.IsPathRooted(root) || !Path.IsPathRooted(path) ||
            !full.StartsWith(fullRoot + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("Path is outside the trusted root");
        string current = Path.GetPathRoot(full);
        foreach (string component in full.Substring(current.Length).Split(Path.DirectorySeparatorChar))
        {
            current = Path.Combine(current, component);
            if (!File.Exists(current) && !Directory.Exists(current))
            {
                if (exists || !String.Equals(current, full, StringComparison.OrdinalIgnoreCase))
                    throw new IOException("Path component does not exist: " + current);
                break;
            }
            if ((File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0)
                throw new InvalidOperationException("Reparse points are not supported: " + current);
        }
        return full;
    }

    static byte[] ReadAll(FileStream stream)
    {
        if (stream.Length > 16 * 1024 * 1024) throw new IOException("Project definition exceeds 16 MiB");
        stream.Position = 0;
        var bytes = new byte[(int)stream.Length];
        int offset = 0;
        while (offset < bytes.Length)
        {
            int count = stream.Read(bytes, offset, bytes.Length - offset);
            if (count == 0) throw new EndOfStreamException();
            offset += count;
        }
        return bytes;
    }

    static void WriteAll(FileStream stream, byte[] bytes)
    {
        stream.Position = 0;
        stream.Write(bytes, 0, bytes.Length);
        stream.SetLength(bytes.Length);
        stream.Flush(true);
    }

    internal static void Execute(TransactionRequest request, Func<string, bool> approve = null)
    {
        if (Environment.OSVersion.Platform != PlatformID.Win32NT)
            throw new PlatformNotSupportedException("Transactions are implemented on Windows only");
        if (request == null || !request.Trusted) throw new InvalidOperationException("Trusted root required");
        if (!Path.IsPathRooted(request.Root)) throw new InvalidOperationException("Trusted root must be absolute");
        Guid id;
        if (!Guid.TryParseExact(request.TransactionId, "D", out id))
            throw new InvalidOperationException("A transaction identifier is required");
        string root = Path.GetFullPath(request.Root);
        string journalPath = CheckedPath(root, request.Journal, request.Recover);
        if (!String.Equals(Path.GetDirectoryName(journalPath), root.TrimEnd(Path.DirectorySeparatorChar),
                           StringComparison.OrdinalIgnoreCase) ||
            !String.Equals(Path.GetFileName(journalPath), ".frost-project-edit-journal", StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("Invalid journal location");
        FileStream journal = null;
        bool durable = request.Recover;
        bool wroteDefinition = false;
        bool created = false;
        var handles = new List<FileStream>();
        var directories = new List<SafeFileHandle>();
        var heldDirectories = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        try
        {
            HoldDirectories(root, directories, heldDirectories);
            journal = new FileStream(OpenChecked(journalPath, false, !request.Recover, true), FileAccess.ReadWrite);
            created = !request.Recover;
            Journal record;
            if (request.Recover)
            {
                byte[] journalBytes = ReadAll(journal);
                string digest;
                using (var sha = SHA256.Create()) digest = BitConverter.ToString(sha.ComputeHash(journalBytes)).Replace("-", "");
                if (String.IsNullOrEmpty(request.JournalDigest) || request.JournalDigest != digest)
                    throw new InvalidOperationException("Journal does not match private recovery record");
                record = Json.Deserialize<Journal>(new UTF8Encoding(false, true).GetString(journalBytes));
                if (record == null || record.Version != 2 || record.TransactionId != request.TransactionId ||
                    !String.Equals(Path.GetFullPath(record.Root), root, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidOperationException("Journal does not match the trusted root");
            }
            else record = new Journal { Version = 2, Root = root, Entries = request.Entries, TransactionId = request.TransactionId };
            if (record.Entries == null || record.Entries.Length != 2)
                throw new InvalidOperationException("Exactly two definition files are required");
            byte[] encodedJournal = Encoding.UTF8.GetBytes(Json.Serialize(record));
            if (encodedJournal.Length > 16 * 1024 * 1024)
                throw new InvalidOperationException("Journal exceeds the recovery read limit");
            foreach (var entry in record.Entries)
                if (Convert.FromBase64String(entry.Before).Length > 16 * 1024 * 1024 ||
                    Convert.FromBase64String(entry.After).Length > 16 * 1024 * 1024)
                    throw new InvalidOperationException("Definition exceeds the recovery read limit");
            string projectPath = Path.GetFullPath(record.Entries[0].Path);
            if (!projectPath.EndsWith(".vcxproj", StringComparison.OrdinalIgnoreCase) ||
                !String.Equals(Path.GetFullPath(record.Entries[1].Path), projectPath + ".filters",
                               StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("Expected a project and its filters file");
            if (request.Recover && (String.IsNullOrEmpty(request.ExpectedProject) ||
                !String.Equals(Path.GetFullPath(request.ExpectedProject), projectPath, StringComparison.OrdinalIgnoreCase)))
                throw new InvalidOperationException("Journal project does not match owned recovery");
            var unique = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var entry in record.Entries)
            {
                string path = CheckedPath(root, entry.Path, true);
                if (!unique.Add(path) || String.Equals(path, journalPath, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidOperationException("Duplicate definition path");
                HoldDirectories(Path.GetDirectoryName(path), directories, heldDirectories);
                handles.Add(new FileStream(OpenChecked(path, false, false, false), FileAccess.ReadWrite));
                FileInformation info;
                if (!GetFileInformationByHandle(handles[handles.Count - 1].SafeFileHandle, out info))
                    throw new IOException("Cannot read file identity");
                if (request.Recover && (entry.Volume != info.Volume || entry.IndexHigh != info.IndexHigh || entry.IndexLow != info.IndexLow))
                    throw new InvalidOperationException("Definition identity changed: " + path);
                entry.Volume = info.Volume;
                entry.IndexHigh = info.IndexHigh;
                entry.IndexLow = info.IndexLow;
            }
            if (!request.Recover)
            {
                if (request.Environment != EnvironmentFingerprint.Current() || request.ImportedInputs == null || request.ImportedInputs.Count == 0)
                    throw new InvalidOperationException("Evaluation environment or imports are missing or changed");
                foreach (var input in request.ImportedInputs)
                {
                    if (!Path.IsPathRooted(input.Key)) throw new InvalidOperationException("Import path must be absolute");
                    HoldDirectories(Path.GetDirectoryName(input.Key), directories, heldDirectories);
                    var stream = new FileStream(OpenChecked(input.Key, false, false, false, true), FileAccess.Read);
                    handles.Add(stream);
                    string actual;
                    using (var sha = SHA256.Create()) actual = BitConverter.ToString(sha.ComputeHash(ReadAll(stream))).Replace("-", "");
                    if (!String.Equals(actual, input.Value, StringComparison.OrdinalIgnoreCase))
                        throw new InvalidOperationException("Imported input changed: " + input.Key);
                }
            }
            for (int i = 0; i < record.Entries.Length; i++)
            {
                byte[] actual = ReadAll(handles[i]);
                byte[] before = Convert.FromBase64String(record.Entries[i].Before);
                byte[] after = Convert.FromBase64String(record.Entries[i].After);
                if (!actual.SequenceEqual(before) && (!request.Recover || !actual.SequenceEqual(after)))
                    throw new InvalidOperationException("Definition changed unexpectedly: " + record.Entries[i].Path);
            }
            if (!request.Recover)
            {
                encodedJournal = Encoding.UTF8.GetBytes(Json.Serialize(record));
                if (encodedJournal.Length > 16 * 1024 * 1024)
                    throw new InvalidOperationException("Journal exceeds the recovery read limit");
                WriteAll(journal, encodedJournal);
                durable = true;
                string digest;
                using (var sha = SHA256.Create()) digest = BitConverter.ToString(sha.ComputeHash(encodedJournal)).Replace("-", "");
                if (approve != null && !approve(digest))
                    throw new InvalidOperationException("Commit rejected");
                for (int i = 0; i < record.Entries.Length; i++)
                {
                    wroteDefinition = true;
                    WriteAll(handles[i], Convert.FromBase64String(record.Entries[i].After));
                    if (request.StopAfterFirst && i == 0)
                        Environment.Exit(86); // Fault injection: process ends without cleanup.
                }
            }
            else {
                if (approve != null && !approve(request.JournalDigest))
                    throw new InvalidOperationException("Recovery rejected");
                for (int i = 0; i < record.Entries.Length; i++)
                    WriteAll(handles[i], Convert.FromBase64String(record.Entries[i].Before));
            }
            DeleteHeldJournal(journal);
        }
        finally
        {
            foreach (var handle in handles) handle.Dispose();
            try
            {
                if (journal != null && created && (!durable || !wroteDefinition)) DeleteHeldJournal(journal);
            }
            finally
            {
                if (journal != null) journal.Dispose();
                foreach (var directory in directories) directory.Dispose();
            }
        }
    }
}
