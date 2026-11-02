// SPDX-License-Identifier: Apache-2.0
// Runs only in the isolated .NET test harness, never as part of the Unity package.
using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using FrostIDE.UnityIntegration;
internal static class UnityPublisherProbe
{
    [DllImport("libc", SetLastError = true)] private static extern int mkfifo(string path, uint mode);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, EntryPoint = "CreateFileW")]
    private static extern IntPtr OpenDirectory(string path, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool DeviceIoControl(IntPtr handle, uint code, byte[] input, uint size, IntPtr output, uint outputSize, out uint returned, IntPtr overlap);
    private static bool MakeJunction(string path, string target)
    {
        IntPtr handle = OpenDirectory(path, 0x40000000, 7, IntPtr.Zero, 3, 0x02200000, IntPtr.Zero);
        if (handle == new IntPtr(-1)) return false;
        try
        {
            string substitute = @"\??\" + target;
            byte[] first = System.Text.Encoding.Unicode.GetBytes(substitute);
            byte[] second = System.Text.Encoding.Unicode.GetBytes(target);
            byte[] buffer = new byte[16 + first.Length + 2 + second.Length + 2];
            void Put16(int offset, int value) { buffer[offset] = (byte)value; buffer[offset + 1] = (byte)(value >> 8); }
            byte[] tag = BitConverter.GetBytes(0xA0000003u); Array.Copy(tag, buffer, 4);
            Put16(4, buffer.Length - 8); Put16(8, 0); Put16(10, first.Length);
            Put16(12, first.Length + 2); Put16(14, second.Length);
            Array.Copy(first, 0, buffer, 16, first.Length); Array.Copy(second, 0, buffer, 18 + first.Length, second.Length);
            return DeviceIoControl(handle, 0x000900A4, buffer, (uint)buffer.Length, IntPtr.Zero, 0, out _, IntPtr.Zero);
        }
        finally { CloseHandle(handle); }
    }
    private static void WindowsPinProbe(string root)
    {
        string target = Path.Combine(root, "target"); string control = Path.Combine(root, "control"); string pinned = Path.Combine(root, "pinned");
        Directory.CreateDirectory(target); Directory.CreateDirectory(control); Directory.CreateDirectory(pinned);
        if (!MakeJunction(control, target)) throw new Exception("control junction creation failed");
        if ((File.GetAttributes(control) & FileAttributes.ReparsePoint) == 0) throw new Exception("control was not a junction");
        string attributesOnly = Path.Combine(root, "attributes-only"); Directory.CreateDirectory(attributesOnly);
        IntPtr weak = OpenDirectory(attributesOnly, 0x80, 1, IntPtr.Zero, 3, 0x02200000, IntPtr.Zero);
        if (weak == new IntPtr(-1)) throw new Exception("attributes-only handle failed");
        try { if (!MakeJunction(attributesOnly, target)) throw new Exception("attributes-only control did not demonstrate conversion"); }
        finally { CloseHandle(weak); }
        using (var pin = SafeControlFile.DirectoryPin.OpenRoot(pinned))
        {
            if (MakeJunction(pinned, target)) throw new Exception("pinned directory was changed to a junction");
            if ((File.GetAttributes(pinned) & FileAttributes.ReparsePoint) != 0) throw new Exception("pinned directory became a junction");
        }
        string alias = Path.Combine(root, "alias"); Directory.CreateDirectory(alias);
        if (!MakeJunction(alias, target)) throw new Exception("alias junction creation failed");
        using (var pin = SafeControlFile.DirectoryPin.OpenRoot(alias))
        using (var child = pin.Child("created", true)) { }
        if (!Directory.Exists(Path.Combine(target, "created"))) throw new Exception("project alias did not resolve to its target");
        Console.WriteLine("Windows attributes-only handle allowed junction conversion; GENERIC_READ pin refused it; project alias resolved");
    }
    private static void Refused(Action action, string label)
    {
        try { action(); } catch (IOException) { Console.WriteLine(label + ": refused"); return; }
        throw new Exception(label + " was not refused");
    }
    private static void Main(string[] args)
    {
        string root = args[0];
        if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows)) { Directory.CreateDirectory(root); WindowsPinProbe(root); return; }
        var files = new SortedDictionary<string, string> { { "FrostUnity.sln", "test" } };
        string Prepare(string name) {
            string output = Path.Combine(root, name, ".frost", "unity");
            Directory.CreateDirectory(Path.GetDirectoryName(output));
            ProjectContext.Publish(output, files, null); return output;
        }
        string refreshOutput = Prepare("refresh");
        string previousRevision = File.ReadAllText(Path.Combine(refreshOutput, "revision"));
        var changedFiles = new SortedDictionary<string, string> { { "FrostUnity.sln", "updated" } };
        ProjectContext.Publish(refreshOutput, changedFiles, null);
        string nextRevision = File.ReadAllText(Path.Combine(refreshOutput, "revision"));
        if (previousRevision == nextRevision ||
            File.ReadAllText(Path.Combine(refreshOutput, "generations", nextRevision.Substring(2, 32), "FrostUnity.sln")) != "updated" ||
            File.ReadAllText(Path.Combine(refreshOutput, "generations", previousRevision.Substring(2, 32), "FrostUnity.sln")) != "test")
            throw new Exception("anchored refresh did not preserve the previous generation");
        Console.WriteLine("anchored refresh retained prior generation");
        string fifoOwnerRoot = Path.Combine(root, "owner");
        Directory.CreateDirectory(fifoOwnerRoot);
        var ensure = typeof(ProjectContext).GetMethod("EnsureOwnedOutput", BindingFlags.Static | BindingFlags.NonPublic);
        ensure.Invoke(null, new object[] { fifoOwnerRoot });
        string owner = Path.Combine(fifoOwnerRoot, ".frost", "unity", "owner");
        File.Delete(owner); if (mkfifo(owner, 384) != 0) throw new Exception("mkfifo owner failed");
        Refused(() => { try { ensure.Invoke(null, new object[] { fifoOwnerRoot }); } catch (TargetInvocationException e) { throw e.InnerException; } }, "owner fifo");
        string revisionOutput = Prepare("revision-fifo");
        string revision = Path.Combine(revisionOutput, "revision");
        File.Delete(revision); if (mkfifo(revision, 384) != 0) throw new Exception("mkfifo revision failed");
        Refused(() => ProjectContext.Publish(revisionOutput, files, null), "revision fifo");
        foreach (string scenario in new[] { "revision", "generations", "generation", "solution" })
        {
            string output = Prepare("link-" + scenario);
            string descriptor = File.ReadAllText(Path.Combine(output, "revision"));
            string generations = Path.Combine(output, "generations");
            string generation = Path.Combine(generations, descriptor.Substring(2, 32));
            string path = scenario == "revision" ? Path.Combine(output, "revision") : scenario == "generations" ? generations :
                scenario == "generation" ? generation : Path.Combine(generation, "FrostUnity.sln");
            string target = path + ".real";
            bool directory = scenario == "generations" || scenario == "generation";
            if (directory) { Directory.Move(path, target); Directory.CreateSymbolicLink(path, target); }
            else { File.Move(path, target); File.CreateSymbolicLink(path, target); }
            Refused(() => ProjectContext.Publish(output, files, null), scenario + " live link");
            if (!File.Exists(directory ? Path.Combine(target, scenario == "generations" ? descriptor.Substring(2, 32) + "/FrostUnity.sln" : "FrostUnity.sln") : target))
                throw new Exception("linked target was modified");
        }
        string aliasTarget = Path.Combine(root, "alias-target");
        string aliasPath = Path.Combine(root, "alias-root");
        Directory.CreateDirectory(aliasTarget);
        Directory.CreateSymbolicLink(aliasPath, aliasTarget);
        ensure.Invoke(null, new object[] { aliasPath });
        if (!File.Exists(Path.Combine(aliasTarget, ".frost", "unity", "owner")))
            throw new Exception("project alias did not retain its identity");
        Console.WriteLine("project alias resolved");
        string swapRoot = Path.Combine(root, "swap");
        string swapParent = Path.Combine(swapRoot, ".frost");
        string swapOutput = Path.Combine(swapParent, "unity");
        string moved = Path.Combine(swapRoot, "moved");
        string attacker = Path.Combine(swapRoot, "attacker");
        Directory.CreateDirectory(swapParent);
        Directory.CreateDirectory(Path.Combine(attacker, "unity"));
        string outside = Path.Combine(attacker, "unity", "revision");
        File.WriteAllText(outside, "preserve");
        bool swapped = false;
        ProjectContext.Publish(swapOutput, files, _ => {
            if (swapped) return;
            swapped = true;
            Directory.Move(swapParent, moved);
            Directory.CreateSymbolicLink(swapParent, attacker);
        });
        if (!swapped || File.ReadAllText(outside) != "preserve" ||
            !File.Exists(Path.Combine(moved, "unity", "revision")))
            throw new Exception("ancestor replacement redirected publication");
        Console.WriteLine("ancestor swap preserved external files");
        Console.WriteLine("publisher FIFO and live-link cases passed");
    }
}
