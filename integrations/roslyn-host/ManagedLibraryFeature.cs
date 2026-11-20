// SPDX-License-Identifier: Apache-2.0
// Real Roslyn symbol admission for package-backed library source, IL and assembly metadata.
using System.Text.Json;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.FindSymbols;
namespace Frost.RoslynHost;
internal sealed class ManagedLibraryFeature : ISemanticFeature
{
    public bool Supports(string method) => method == "frost/managed/librarySource";
    public async Task<object?> Execute(string method, SemanticFeatureContext context, JsonElement p, CancellationToken token)
    {
        var doc = context.Document;
        var text = await doc.GetTextAsync(token);
        var position = p.GetProperty("position");
        int line = position.GetProperty("line").GetInt32(), column = position.GetProperty("character").GetInt32();
        if (line < 0 || line >= text.Lines.Count || column < 0 || column > text.Lines[line].Span.Length)
            throw new InvalidDataException("Library navigation position is outside the current UTF-16 document");
        var symbol = await SymbolFinder.FindSymbolAtPositionAsync(doc, text.Lines[line].Start + column, token);
        if (symbol is null || !symbol.Locations.Any(location => location.IsInMetadata)) return null;
        symbol = symbol.OriginalDefinition;
        var compilation = await doc.Project.GetCompilationAsync(token) ?? throw new InvalidOperationException("Compilation unavailable");
        var matches = compilation.References.OfType<PortableExecutableReference>().Where(reference =>
            SymbolEqualityComparer.Default.Equals(compilation.GetAssemblyOrModuleSymbol(reference), symbol.ContainingAssembly)).ToArray();
        if (matches.Length != 1 || matches[0].FilePath is not string assembly || !Path.IsPathFullyQualified(assembly))
            throw new InvalidOperationException("Library symbol has no unique evaluated assembly reference");
        string mode = p.TryGetProperty("mode", out var selected) ? selected.GetString() ?? "auto" : "auto";
        // Only the explicit source-link action supplies this opt-in. Ordinary F12 never uses network.
        bool allowSourceLink = p.TryGetProperty("allowSourceLink", out var allow) && allow.ValueKind == JsonValueKind.True;
        var referenceMetadata = matches[0].GetMetadata() as AssemblyMetadata
            ?? throw new NotSupportedException("Library reference is not an assembly");
        var modules = referenceMetadata.GetModules();
        if (modules.Length != 1) throw new NotSupportedException("Multi-module library references are not supported");
        return await LibrarySources.Read(symbol, assembly, mode, allowSourceLink, token, modules[0].GetModuleVersionId());
    }
}
