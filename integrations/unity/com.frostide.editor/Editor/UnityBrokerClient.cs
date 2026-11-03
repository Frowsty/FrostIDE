// SPDX-License-Identifier: Apache-2.0
// Current-user authenticated Unity client for FrostIDE's project-scoped IPC broker.
using System;
using System.IO;
using System.Diagnostics;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace FrostIDE.UnityIntegration
{
    internal static class UnityBrokerClient
    {
        internal sealed class BrokerFailure : IOException
        {
            internal readonly string Stage, Kind;
            internal BrokerFailure(string stage, string kind) : base("Unity broker unavailable")
            {
                Stage = stage.Length <= 32 ? stage : stage.Substring(0, 32);
                Kind = kind.Length <= 64 ? kind : kind.Substring(0, 64);
            }
        }
        [DllImport("libc", SetLastError = true)] private static extern uint geteuid();
        [DllImport("libc", SetLastError = true)] private static extern IntPtr realpath(string path, IntPtr output);
        [DllImport("libc")] private static extern void free(IntPtr value);
        [DllImport("libc", SetLastError = true)] private static extern int getsockopt(IntPtr fd, int level, int name, out Peer peer, ref uint length);
        [StructLayout(LayoutKind.Sequential)] private struct Peer { internal int pid; internal uint uid, gid; }
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, EntryPoint = "WaitNamedPipeW")]
        private static extern bool WaitNamedPipe(string name, uint timeout);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, EntryPoint = "CreateFileW")]
        private static extern IntPtr OpenPipe(string name, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);
        [DllImport("kernel32.dll", SetLastError = true)] private static extern bool SetNamedPipeHandleState(IntPtr pipe, ref uint mode, IntPtr collection, IntPtr timeout);
        [DllImport("kernel32.dll", SetLastError = true)] private static extern bool ReadFile(IntPtr pipe, IntPtr buffer, uint count, out uint transferred, IntPtr overlap);
        [DllImport("kernel32.dll", SetLastError = true)] private static extern bool WriteFile(IntPtr pipe, IntPtr buffer, uint count, out uint transferred, IntPtr overlap);
        [DllImport("kernel32.dll")] private static extern IntPtr GetCurrentProcess();
        [DllImport("kernel32.dll")] private static extern IntPtr LocalFree(IntPtr memory);
        [DllImport("advapi32.dll", SetLastError = true)] private static extern bool GetTokenInformation(IntPtr token, int kind, IntPtr value, uint length, out uint required);
        [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true, EntryPoint = "ConvertSidToStringSidW")]
        private static extern bool ConvertSidToStringSid(IntPtr sid, out IntPtr text);
        [DllImport("kernel32.dll", SetLastError = true)] private static extern bool GetNamedPipeServerProcessId(IntPtr pipe, out uint pid);
        [DllImport("kernel32.dll", SetLastError = true)] private static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
        [DllImport("advapi32.dll", SetLastError = true)] private static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
        [DllImport("kernel32.dll")] private static extern bool CloseHandle(IntPtr handle);

        internal static string PhysicalProject(string path)
        {
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
                using (var root = SafeControlFile.DirectoryPin.OpenRoot(path)) return root.Path.Replace('\\', '/');
            IntPtr pointer = realpath(path, IntPtr.Zero);
            if (pointer == IntPtr.Zero) throw new IOException("Cannot resolve Unity project root");
            try
            {
                int length = 0;
                while (length < 32768 && Marshal.ReadByte(pointer, length) != 0) ++length;
                if (length == 32768) throw new IOException("Unity project path is too long");
                var bytes = new byte[length]; Marshal.Copy(pointer, bytes, 0, length);
                return new UTF8Encoding(false, true).GetString(bytes);
            }
            finally { free(pointer); }
        }
        internal static string ProjectKey(string canonical)
        {
            string value = canonical;
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            {
                var normalized = new StringBuilder(value.Length);
                foreach (char c in value)
                    normalized.Append(c == '/' ? '\\' : c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c);
                value = normalized.ToString();
            }
            using (var sha = SHA256.Create())
            {
                var result = new StringBuilder(64);
                foreach (byte b in sha.ComputeHash(Encoding.UTF8.GetBytes(value))) result.Append(b.ToString("x2"));
                return result.ToString();
            }
        }
        private static string UserSid(IntPtr process)
        {
            if (!OpenProcessToken(process, 0x8, out IntPtr token)) throw new IOException("Cannot query broker user");
            try
            {
                GetTokenInformation(token, 1, IntPtr.Zero, 0, out uint required);
                if (required == 0 || required > 65536) throw new IOException("Invalid broker user information");
                IntPtr information = Marshal.AllocHGlobal((int)required);
                try
                {
                    if (!GetTokenInformation(token, 1, information, required, out _)) throw new IOException("Cannot query broker user");
                    IntPtr sid = Marshal.ReadIntPtr(information);
                    if (sid == IntPtr.Zero || !ConvertSidToStringSid(sid, out IntPtr text)) throw new IOException("Cannot format broker user");
                    try { return Marshal.PtrToStringUni(text) ?? throw new IOException("Empty broker user"); }
                    finally { LocalFree(text); }
                }
                finally { Marshal.FreeHGlobal(information); }
            }
            finally { CloseHandle(token); }
        }
        private static bool CurrentUserServer(IntPtr pipe, string expected)
        {
            if (!GetNamedPipeServerProcessId(pipe, out uint pid)) return false;
            IntPtr process = OpenProcess(0x1000, false, pid);
            if (process == IntPtr.Zero) return false;
            try { return String.Equals(expected, UserSid(process), StringComparison.Ordinal); }
            finally { CloseHandle(process); }
        }
        // PIPE_NOWAIT keeps synchronous native reads and writes bounded on Unity's Mono runtime.
        private static void PipeTransfer(IntPtr pipe, byte[] data, bool write, Stopwatch deadline, Func<bool> cancelled)
        {
            var pin = GCHandle.Alloc(data, GCHandleType.Pinned);
            try
            {
                int used = 0;
                while (used < data.Length)
                {
                    if (deadline.ElapsedMilliseconds >= 2000 || (cancelled != null && cancelled()))
                        throw new IOException("Unity broker I/O timed out or was cancelled");
                    IntPtr at = IntPtr.Add(pin.AddrOfPinnedObject(), used);
                    bool ok = write ? WriteFile(pipe, at, (uint)(data.Length - used), out uint count, IntPtr.Zero)
                                    : ReadFile(pipe, at, (uint)(data.Length - used), out count, IntPtr.Zero);
                    if (ok && count > 0) { used += (int)count; continue; }
                    int error = ok ? 232 : Marshal.GetLastWin32Error();
                    if (error != 232 && error != 536) throw new IOException("Unity broker pipe I/O failed");
                    Thread.Sleep(10);
                }
            }
            finally { pin.Free(); }
        }
        private static void PipeExchange(IntPtr pipe, byte[] request, Func<bool> cancelled)
        {
            var deadline = Stopwatch.StartNew();
            byte[] header = BitConverter.GetBytes(request.Length);
            if (!BitConverter.IsLittleEndian) Array.Reverse(header);
            try { PipeTransfer(pipe, header, true, deadline, cancelled); PipeTransfer(pipe, request, true, deadline, cancelled); }
            catch (Exception error) { throw new BrokerFailure("frame-write", error.GetType().Name); }
            byte[] responseHeader = new byte[4];
            try { PipeTransfer(pipe, responseHeader, false, deadline, cancelled); }
            catch (Exception error) { throw new BrokerFailure("frame-read", error.GetType().Name); }
            if (!BitConverter.IsLittleEndian) Array.Reverse(responseHeader);
            int size = BitConverter.ToInt32(responseHeader, 0);
            if (size < 1 || size > 65537) throw new IOException("Invalid Unity broker response");
            byte[] response = new byte[size];
            try { PipeTransfer(pipe, response, false, deadline, cancelled); }
            catch (Exception error) { throw new BrokerFailure("frame-read", error.GetType().Name); }
            try { PipeTransfer(pipe, new byte[] { 1 }, true, deadline, cancelled); }
            catch (Exception error) { throw new BrokerFailure("frame-write", error.GetType().Name); }
            if (response[0] != 0) throw new IOException("Unity session was not accepted");
        }
        private static void Wait(Task task, Stopwatch deadline, Func<bool> cancelled)
        {
            while (!task.IsCompleted)
            {
                long remaining = CheckDeadline(deadline, cancelled);
                task.Wait((int)Math.Min(50, remaining));
            }
            CheckDeadline(deadline, cancelled);
            task.GetAwaiter().GetResult();
        }
        private static long CheckDeadline(Stopwatch deadline, Func<bool> cancelled)
        {
            long remaining = 2000 - deadline.ElapsedMilliseconds;
            if (remaining <= 0 || (cancelled != null && cancelled())) throw new IOException("Unity broker I/O timed out or was cancelled");
            return remaining;
        }
        private static void Write(Stream stream, byte[] data, Stopwatch deadline, Func<bool> cancelled)
        {
            try { CheckDeadline(deadline, cancelled); Wait(stream.WriteAsync(data, 0, data.Length), deadline, cancelled); }
            catch (Exception error) { throw new BrokerFailure("frame-write", error.GetType().Name); }
        }
        private static byte[] Read(Stream stream, int length, Stopwatch deadline, Func<bool> cancelled)
        {
            try
            {
                var data = new byte[length]; int used = 0;
                while (used < length)
                {
                    CheckDeadline(deadline, cancelled);
                    Task<int> task = stream.ReadAsync(data, used, length - used);
                    Wait(task, deadline, cancelled);
                    int count = task.GetAwaiter().GetResult();
                    if (count <= 0) throw new IOException("Unity broker closed");
                    used += count;
                }
                return data;
            }
            catch (Exception error) { throw new BrokerFailure("frame-read", error.GetType().Name); }
        }
        private static void Exchange(Stream stream, byte[] request, Func<bool> cancelled)
        {
            if (request.Length > 65536) throw new IOException("Unity broker message too large");
            var deadline = Stopwatch.StartNew();
            var header = BitConverter.GetBytes(request.Length);
            if (!BitConverter.IsLittleEndian) Array.Reverse(header);
            Write(stream, header, deadline, cancelled); Write(stream, request, deadline, cancelled);
            var responseHeader = Read(stream, 4, deadline, cancelled);
            if (!BitConverter.IsLittleEndian) Array.Reverse(responseHeader);
            int size = BitConverter.ToInt32(responseHeader, 0);
            if (size < 1 || size > 65537) throw new IOException("Invalid Unity broker response");
            byte[] response = Read(stream, size, deadline, cancelled); Write(stream, new byte[] { 1 }, deadline, cancelled);
            if (response[0] != 0) throw new IOException("Unity session was not accepted");
        }
        internal static void Announce(string project, string json, Func<bool> cancelled = null)
        {
            string stage = "identity";
            try { AnnounceUnchecked(project, json, value => stage = value, cancelled); }
            catch (BrokerFailure) { throw; }
            catch (Exception error) { throw new BrokerFailure(stage, error.GetType().Name); }
        }
        private static void AnnounceUnchecked(string project, string json, Action<string> stage, Func<bool> cancelled)
        {
            string key = ProjectKey(project);
            byte[] request = Encoding.UTF8.GetBytes(json);
            if (request.Length > 65536) throw new IOException("Unity broker message too large");
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            {
                stage("current-user"); string sid = UserSid(GetCurrentProcess());
                string name = @"\\.\pipe\FrostIDE-" + sid + "-" + key;
                stage("connect");
                if (!WaitNamedPipe(name, 200)) throw new IOException("Unity broker is unavailable");
                IntPtr pipe = OpenPipe(name, 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
                if (pipe == new IntPtr(-1)) throw new IOException("Cannot open Unity broker");
                try
                {
                    stage("peer"); if (!CurrentUserServer(pipe, sid)) throw new IOException("Unity broker belongs to another user");
                    uint mode = 1; // PIPE_READMODE_BYTE | PIPE_NOWAIT
                    stage("pipe-mode"); if (!SetNamedPipeHandleState(pipe, ref mode, IntPtr.Zero, IntPtr.Zero))
                        throw new IOException("Cannot configure Unity broker pipe");
                    stage("frame"); PipeExchange(pipe, request, cancelled);
                }
                finally { CloseHandle(pipe); }
                return;
            }
            if (!RuntimeInformation.IsOSPlatform(OSPlatform.Linux)) throw new IOException("Unity controls unsupported on this platform");
            string path = "/tmp/frostide-ipc-" + geteuid() + "/" + key;
            using (var socket = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified))
            {
                stage("connect"); socket.Blocking = false;
                try { socket.Connect(new UnixDomainSocketEndPoint(path)); }
                catch (SocketException e) when (e.SocketErrorCode == SocketError.WouldBlock || e.SocketErrorCode == SocketError.InProgress) { }
                if (!socket.Poll(200000, SelectMode.SelectWrite) || (int)socket.GetSocketOption(SocketOptionLevel.Socket, SocketOptionName.Error) != 0)
                    throw new IOException("Unity broker connection timed out");
                stage("peer"); uint size = (uint)Marshal.SizeOf<Peer>();
                if (getsockopt(socket.Handle, 1, 17, out Peer peer, ref size) != 0 || peer.uid != geteuid())
                    throw new IOException("Unity broker belongs to another user");
                socket.Blocking = true;
                stage("frame"); using (var stream = new NetworkStream(socket, false)) Exchange(stream, request, cancelled);
            }
        }
    }
}
