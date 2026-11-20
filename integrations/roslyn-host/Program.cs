// SPDX-License-Identifier: Apache-2.0
// Register the actual installed SDK before loading any MSBuildWorkspace assembly.
using System.Runtime.CompilerServices;
using Microsoft.Build.Locator;
namespace Frost.RoslynHost;
internal static class Program
{
    public static async Task<int> Main(string[] args)
    {
        try { MSBuildLocator.RegisterDefaults(); return await Run(args); }
        catch (Exception error) { await Console.Error.WriteLineAsync(error.ToString()); return 1; }
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static Task<int> Run(string[] args) => new LanguageHost(args).Run();
}
