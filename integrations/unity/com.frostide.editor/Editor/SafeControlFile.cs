// SPDX-License-Identifier: Apache-2.0
// Bounded, no-follow reads for the small files owned by the Unity bridge.
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace FrostIDE.UnityIntegration
{
    internal static class SafeControlFile
    {
        [DllImport("libc", SetLastError = true, EntryPoint = "open")]
        private static extern int Open(string path, int flags);
        [DllImport("libc", SetLastError = true, EntryPoint = "openat")]
        private static extern int OpenAt(int fd, string path, int flags);
        [DllImport("libc", SetLastError = true, EntryPoint = "mkdirat")]
        private static extern int MkdirAt(int fd, string path, uint mode);
        [DllImport("libc", SetLastError = true, EntryPoint = "close")]
        private static extern int Close(int fd);
        [DllImport("libc", SetLastError = true, EntryPoint = "read")]
        private static extern IntPtr Read(int fd, byte[] buffer, IntPtr count);
        [DllImport("libc", SetLastError = true, EntryPoint = "statx")]
        private static extern int Statx(int fd, string path, int flags, uint mask, IntPtr result);
        [DllImport("libc", SetLastError = true, EntryPoint = "fstat")]
        private static extern int Fstat(int fd, IntPtr result);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, EntryPoint = "CreateFileW")]
        private static extern IntPtr CreateFile(string path, uint access, uint share, IntPtr security,
            uint creation, uint flags, IntPtr template);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, EntryPoint = "CreateDirectoryW")]
        private static extern bool CreateDirectoryNative(string path, IntPtr security);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool CloseHandle(IntPtr handle);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern uint GetFileType(IntPtr handle);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool GetFileInformationByHandle(IntPtr handle, out FileInfo info);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, EntryPoint = "GetFinalPathNameByHandleW")]
        private static extern uint GetFinalPathNameByHandle(IntPtr handle, StringBuilder path, uint size, uint flags);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool ReadFile(IntPtr handle, byte[] buffer, uint size, out uint count, IntPtr overlapped);
        [StructLayout(LayoutKind.Sequential)]
        private struct FileInfo
        {
            internal uint Attributes, CreationLow, CreationHigh, AccessLow, AccessHigh, WriteLow, WriteHigh;
            internal uint Volume, SizeHigh, SizeLow, Links, IndexHigh, IndexLow;
        }

        // Paths below a pinned directory resolve through its descriptor on POSIX,
        // so renaming an ancestor cannot redirect subsequent reads or writes. On
        // Windows each handle denies delete sharing until publication is finished.
        internal sealed class DirectoryPin : IDisposable
        {
            private readonly int fd;
            private readonly IntPtr handle;
            internal readonly string Path;
            private DirectoryPin(int descriptor, IntPtr value, string path) { fd = descriptor; handle = value; Path = path; }
            public void Dispose()
            {
                if (fd >= 0) Close(fd);
                if (handle != IntPtr.Zero && handle != new IntPtr(-1)) CloseHandle(handle);
            }
            private static bool Windows => RuntimeInformation.IsOSPlatform(OSPlatform.Windows);
            private static bool Mac => RuntimeInformation.IsOSPlatform(OSPlatform.OSX);
            private static int DirectoryFlags => Mac ? 0x01100104 : 0x000B0800; // CLOEXEC | DIRECTORY | NOFOLLOW | NONBLOCK
            private static DirectoryPin FromUnix(int descriptor)
            {
                if (descriptor < 0) throw new IOException("Cannot pin Unity directory", new Win32Exception(Marshal.GetLastWin32Error()));
                string anchor = (Mac ? "/dev/fd/" : "/proc/self/fd/") + descriptor;
                if (!Directory.Exists(anchor)) { Close(descriptor); throw new IOException("Directory descriptors are unavailable"); }
                return new DirectoryPin(descriptor, IntPtr.Zero, anchor);
            }
            private static DirectoryPin FromWindows(string path, bool followRoot = false)
            {
                IntPtr value = CreateFile(path, 0x80000000, 1, IntPtr.Zero, 3, followRoot ? 0x02000000u : 0x02200000u, IntPtr.Zero);
                if (value == new IntPtr(-1)) throw new IOException("Cannot pin Unity directory", new Win32Exception(Marshal.GetLastWin32Error()));
                if (GetFileType(value) != 1 || !GetFileInformationByHandle(value, out FileInfo info) ||
                    (info.Attributes & 0x400u) != 0 || (info.Attributes & 0x10u) == 0)
                { CloseHandle(value); throw new IOException("Unity directory is linked or invalid: " + path); }
                if (followRoot)
                {
                    var resolved = new StringBuilder(32768);
                    uint length = GetFinalPathNameByHandle(value, resolved, (uint)resolved.Capacity, 0);
                    if (length == 0 || length >= resolved.Capacity) { CloseHandle(value); throw new IOException("Cannot resolve Unity project root"); }
                    path = resolved.ToString();
                    if (path.StartsWith(@"\\?\UNC\", StringComparison.OrdinalIgnoreCase)) path = @"\\" + path.Substring(8);
                    else if (path.StartsWith(@"\\?\", StringComparison.OrdinalIgnoreCase)) path = path.Substring(4);
                }
                return new DirectoryPin(-1, value, path);
            }
            internal static DirectoryPin OpenRoot(string path)
            {
                if (Windows) return FromWindows(path, true);
                if (!Mac && !RuntimeInformation.IsOSPlatform(OSPlatform.Linux)) throw new IOException("Unsupported Unity platform");
                return FromUnix(Open(path, DirectoryFlags & ~(Mac ? 0x100 : 0x20000)));
            }
            internal DirectoryPin Child(string name, bool create, bool exclusive = false)
            {
                if (String.IsNullOrEmpty(name) || name == "." || name == ".." ||
                    name.IndexOfAny(new[] { '/', '\\' }) >= 0) throw new IOException("Invalid Unity directory component");
                if (Windows)
                {
                    string path = System.IO.Path.Combine(Path, name);
                    if (create && exclusive && !CreateDirectoryNative(path, IntPtr.Zero))
                        throw new IOException("Cannot create unique Unity directory", new Win32Exception(Marshal.GetLastWin32Error()));
                    if (create && !exclusive) Directory.CreateDirectory(path);
                    return FromWindows(path);
                }
                if (create && MkdirAt(fd, name, 448) != 0 && (exclusive || Marshal.GetLastWin32Error() != 17)) // EEXIST
                    throw new IOException("Cannot create Unity directory", new Win32Exception(Marshal.GetLastWin32Error()));
                return FromUnix(OpenAt(fd, name, DirectoryFlags));
            }
        }

        // Null denotes a missing file; every other unsupported or unsafe path fails closed.
        internal static string ReadRegular(string path, bool contents = true)
        {
            var data = new byte[69];
            int count = 0;
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            {
                IntPtr handle = CreateFile(path, 0x80000000, 3, IntPtr.Zero, 3, 0x00200000, IntPtr.Zero);
                if (handle == new IntPtr(-1))
                {
                    int error = Marshal.GetLastWin32Error();
                    if (error == 2 || error == 3) return null;
                    throw new IOException("Cannot open Unity control file", new Win32Exception(error));
                }
                try
                {
                    if (GetFileType(handle) != 1 || !GetFileInformationByHandle(handle, out FileInfo info) ||
                        (info.Attributes & (0x10u | 0x400u)) != 0)
                        throw new IOException("Unity control file is not a regular file: " + path);
                    if (contents)
                    {
                        if (!ReadFile(handle, data, (uint)data.Length, out uint read, IntPtr.Zero))
                            throw new IOException("Cannot read Unity control file", new Win32Exception(Marshal.GetLastWin32Error()));
                        count = (int)read;
                    }
                }
                finally { CloseHandle(handle); }
            }
            else
            {
                bool mac = RuntimeInformation.IsOSPlatform(OSPlatform.OSX);
                if (!mac && !RuntimeInformation.IsOSPlatform(OSPlatform.Linux))
                    throw new IOException("Unsupported platform for Unity control files");
                int fd = Open(path, mac ? 0x01000104 : 0x000A0800); // CLOEXEC | NOFOLLOW | NONBLOCK
                if (fd < 0)
                {
                    int error = Marshal.GetLastWin32Error();
                    if (error == 2) return null;
                    throw new IOException("Cannot open Unity control file", new Win32Exception(error));
                }
                IntPtr stat = Marshal.AllocHGlobal(512);
                try
                {
                    int result = mac ? Fstat(fd, stat) : Statx(fd, "", 0x1000, 1, stat); // AT_EMPTY_PATH, STATX_TYPE
                    int mode = result == 0 ? (ushort)Marshal.ReadInt16(stat, mac ? 4 : 28) : 0;
                    if (result != 0 || (mode & 0xf000) != 0x8000)
                        throw new IOException("Unity control file is not a regular file: " + path);
                    if (contents)
                    {
                        long read = Read(fd, data, (IntPtr)data.Length).ToInt64();
                        if (read < 0) throw new IOException("Cannot read Unity control file", new Win32Exception(Marshal.GetLastWin32Error()));
                        count = (int)read;
                    }
                }
                finally { Marshal.FreeHGlobal(stat); Close(fd); }
            }
            if (count > 68) throw new IOException("Unity control file is too large: " + path);
            return contents ? new UTF8Encoding(false, true).GetString(data, 0, count) : String.Empty;
        }
    }
}
