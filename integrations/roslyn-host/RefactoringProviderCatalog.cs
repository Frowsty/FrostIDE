// SPDX-License-Identifier: Apache-2.0
// Allowlisted installed Roslyn provider exports share the workspace's actual MEF composition.
using System.Composition;
using System.Composition.Hosting;
using System.Reflection;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CodeRefactorings;
using Microsoft.CodeAnalysis.Host.Mef;
namespace Frost.RoslynHost;
internal sealed class RefactoringProviderCatalog : IDisposable
{
    private readonly CompositionHost composition;
    private readonly Imports imports = new();
    internal MefHostServices HostServices { get; }
    private sealed class Imports
    {
        [ImportMany]
        public IEnumerable<Lazy<CodeRefactoringProvider, IDictionary<string, object>>> Providers { get; set; } = [];
    }
    internal RefactoringProviderCatalog(IEnumerable<Assembly> assemblies)
    {
        // Same public MEF composition contract used by MefHostServices.Create(assemblies).
        // https://github.com/dotnet/roslyn/blob/main/src/Workspaces/Core/Portable/Workspace/Host/Mef/MefHostServices.cs
        composition = new ContainerConfiguration().WithAssemblies(assemblies.Distinct()).CreateContainer();
        HostServices = MefHostServices.Create(composition);
        composition.SatisfyImports(imports);
    }
    internal IEnumerable<CodeRefactoringProvider> Providers(string name)
    {
        foreach (var export in imports.Providers) {
            if (!export.Metadata.TryGetValue("Name", out var candidate) || candidate is not string actual || actual != name ||
                !export.Metadata.TryGetValue("Languages", out var languages) || languages is not IEnumerable<string> supported ||
                !supported.Contains(LanguageNames.CSharp, StringComparer.Ordinal)) continue;
            yield return export.Value;
        }
    }
    public void Dispose() => composition.Dispose();
}
