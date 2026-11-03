// SPDX-License-Identifier: Apache-2.0
// Verify Unity broker failures report a bounded stage and type without request secrets.
using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Threading.Tasks;
using FrostIDE.UnityIntegration;
internal static class UnityBrokerDiagnosticProbe
{
    private static void Main()
    {
        try { UnityBrokerClient.Announce("/missing-unity-diagnostic-project", new string('x', 65537)); }
        catch (UnityBrokerClient.BrokerFailure error)
        {
            if (error.Stage != "identity" || error.Kind != "IOException") throw;
            goto CheckDeadline;
        }
        throw new Exception("oversized announcement accepted");
    CheckDeadline:
        var wait = typeof(UnityBrokerClient).GetMethod("Wait", BindingFlags.NonPublic | BindingFlags.Static);
        try { wait.Invoke(null, new object[] { Task.CompletedTask, Stopwatch.StartNew(), (Func<bool>)(() => true) }); }
        catch (TargetInvocationException error) when (error.InnerException is IOException) { goto CheckExpiry; }
        throw new Exception("completed operation ignored cancellation");
    CheckExpiry:
        var expired = Stopwatch.StartNew();
        System.Threading.Thread.Sleep(2050);
        try { wait.Invoke(null, new object[] { Task.CompletedTask, expired, (Func<bool>)(() => false) }); }
        catch (TargetInvocationException error) when (error.InnerException is IOException) { goto CheckWrite; }
        throw new Exception("completed operation ignored deadline");
    CheckWrite:
        var write = typeof(UnityBrokerClient).GetMethod("Write", BindingFlags.NonPublic | BindingFlags.Static);
        using (var stream = new MemoryStream())
        {
            try { write.Invoke(null, new object[] { stream, new byte[] { 1 }, Stopwatch.StartNew(), (Func<bool>)(() => true) }); }
            catch (TargetInvocationException error) when (error.InnerException is UnityBrokerClient.BrokerFailure)
            {
                if (stream.Length != 0) throw new Exception("cancelled write started");
                goto CheckDiagnostic;
            }
            throw new Exception("cancelled write accepted");
        }
    CheckDiagnostic:
        string secret = new string('s', 64);
        try { UnityBrokerClient.Announce("/missing-unity-diagnostic-project", secret); }
        catch (UnityBrokerClient.BrokerFailure error)
        {
            if (error.Stage != "connect" || String.IsNullOrEmpty(error.Kind) || error.Kind.Length > 100 ||
                error.ToString().Contains(secret)) throw new Exception("unsafe broker diagnostic");
            Console.WriteLine("Unity broker diagnostic reports connect stage without request contents");
            return;
        }
        throw new Exception("unexpected broker acceptance");
    }
}
