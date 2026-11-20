// SPDX-License-Identifier: Apache-2.0
// Actual Roslyn/MSBuildWorkspace semantics over trusted SDK projects and versioned editor overlays.
using System.Collections.Immutable;
using System.Reflection;
using System.Text.Json;
using System.Xml.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.Completion;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.FindSymbols;
using Microsoft.CodeAnalysis.Formatting;
using Microsoft.CodeAnalysis.Host.Mef;
using Microsoft.CodeAnalysis.MSBuild;
using Microsoft.CodeAnalysis.QuickInfo;
using Microsoft.CodeAnalysis.Rename;
using Microsoft.CodeAnalysis.Simplification;
using Microsoft.CodeAnalysis.Text;
namespace Frost.RoslynHost;
internal sealed class LanguageHost
{
    private readonly RpcTransport rpc = new();
    private readonly CancellationTokenSource stop = new();
    private readonly string[] args;
    private readonly ISemanticFeature[] semanticFeatures = typeof(LanguageHost).Assembly.GetTypes()
        .Where(type => type.IsClass && !type.IsAbstract && typeof(ISemanticFeature).IsAssignableFrom(type))
        .Select(type => (ISemanticFeature)Activator.CreateInstance(type, nonPublic: true)!).ToArray();
    private readonly Dictionary<string, int> versions = new(PathComparer);
    private static StringComparer PathComparer => OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;
    private MSBuildWorkspace? workspace;
    private RefactoringProviderCatalog? providerCatalog;
    private Solution? solution;
    private string root = "";
    private bool shutdown;
    internal LanguageHost(string[] arguments) { args = arguments; }
    internal async Task<int> Run()
    {
        var reading = rpc.Read(stop.Token);
        var consuming = rpc.Consume(Dispatch, stop.Token);
        try { var completed = await Task.WhenAny(reading, consuming); if (completed == consuming) stop.Cancel(); await Task.WhenAll(reading, consuming); }
        catch (OperationCanceledException) when (stop.IsCancellationRequested) { }
        finally { stop.Cancel(); workspace?.Dispose(); providerCatalog?.Dispose(); }
        return shutdown ? 0 : 1;
    }
    private string? Option(string name)
    {
        var index = Array.IndexOf(args, name);
        if (index < 0) return null;
        if (index + 1 >= args.Length || string.IsNullOrWhiteSpace(args[index + 1])) throw new ArgumentException(name + " requires a value");
        return args[index + 1];
    }
    private async Task Load(CancellationToken token)
    {
        var target = Option("--solution");
        if (target is null) {
            var candidates = Directory.EnumerateFiles(root, "*", SearchOption.TopDirectoryOnly)
                .Where(p => new[] { ".sln", ".slnx", ".csproj" }.Contains(Path.GetExtension(p), StringComparer.OrdinalIgnoreCase)).ToArray();
            var solutions = candidates.Where(p => !p.EndsWith(".csproj", StringComparison.OrdinalIgnoreCase)).ToArray();
            if (solutions.Length == 1) target = solutions[0];
            else if (solutions.Length == 0 && candidates.Length == 1) target = candidates[0];
            else throw new InvalidOperationException("Select an unambiguous solution or SDK project for RoslynHost");
        }
        target = Path.GetFullPath(target);
        if (!File.Exists(target)) throw new FileNotFoundException("Selected managed project is unavailable", target);
        var properties = new Dictionary<string, string> { ["Configuration"] = Option("--configuration") ?? "Debug" };
        if (Option("--framework") is string framework) properties["TargetFramework"] = framework;
        if (Option("--runtime-identifier") is string runtimeIdentifier) properties["RuntimeIdentifier"] = runtimeIdentifier;
        var assemblies = MefHostServices.DefaultAssemblies
            .Add(Assembly.Load("Microsoft.CodeAnalysis.CSharp.Workspaces"))
            .Add(Assembly.Load("Microsoft.CodeAnalysis.Features"))
            .Add(Assembly.Load("Microsoft.CodeAnalysis.CSharp.Features"));
        providerCatalog = new RefactoringProviderCatalog(assemblies);
        workspace = MSBuildWorkspace.Create(properties, providerCatalog.HostServices);
        workspace.LoadMetadataForReferencedProjects = false;
        workspace.SkipUnrecognizedProjects = false;
        workspace.WorkspaceFailed += (_, value) => Console.Error.WriteLine(value.Diagnostic.Message);
        solution = target.EndsWith(".csproj", StringComparison.OrdinalIgnoreCase)
            ? (await workspace.OpenProjectAsync(target, cancellationToken: token)).Solution
            : await workspace.OpenSolutionAsync(target, cancellationToken: token);
        if (!solution.Projects.Any(p => p.Language == LanguageNames.CSharp)) throw new InvalidOperationException("Selected workspace has no evaluated C# projects");
        await rpc.Notify("window/logMessage", new { type = 3, message = $"RoslynHost loaded {solution.ProjectIds.Count} evaluated projects" });
    }
    private static string FileOf(JsonElement value) {
        var uri = new Uri(value.GetProperty("uri").GetString() ?? throw new InvalidDataException("Missing document URI"));
        if (!uri.IsAbsoluteUri || !uri.IsFile) throw new InvalidDataException("Managed document requires an absolute file URI");
        return uri.LocalPath;
    }
    private static ImmutableArray<DocumentId> SourceIds(Solution captured, string file) =>
        captured.GetDocumentIdsWithFilePath(file).Where(id => captured.GetDocument(id)?.Project.Language == LanguageNames.CSharp).ToImmutableArray();
    private Document Document(JsonElement parameters)
    {
        if (solution is null) throw new InvalidOperationException("Roslyn workspace has not loaded");
        var file = FileOf(parameters.GetProperty("textDocument"));
        var ids = SourceIds(solution, file);
        if (ids.Length == 0) throw new InvalidOperationException("Document is outside the evaluated Compile items");
        if (ids.Length == 1) return solution.GetDocument(ids[0])!;
        var project = Option("--project-context");
        var candidates = ids.Select(id => solution.GetDocument(id)!).Where(d => project is null || PathComparer.Equals(d.Project.FilePath, Path.GetFullPath(project))).ToArray();
        if (candidates.Length != 1) throw new InvalidOperationException("Linked/multitarget document needs an explicit project context");
        return candidates[0];
    }
    private static int Offset(SourceText text, JsonElement position)
    {
        var line = position.GetProperty("line").GetInt32(); var column = position.GetProperty("character").GetInt32();
        if (line < 0 || line >= text.Lines.Count || column < 0 || column > text.Lines[line].Span.Length) throw new InvalidDataException("Position outside UTF-16 document");
        return text.Lines[line].Start + column;
    }
    private static object Position(SourceText text, int offset) { var p = text.Lines.GetLinePosition(offset); return new { line = p.Line, character = p.Character }; }
    private static object Range(SourceText text, TextSpan span) => new { start = Position(text, span.Start), end = Position(text, span.End) };
    private static string UriOf(string file) => new Uri(Path.GetFullPath(file)).AbsoluteUri;
    private SemanticFeatureContext Context(Document document) => new(document, async (changed, token) => {
        var captured = document.Project.Solution; await ValidateSources(captured, token);
        foreach (var project in changed.GetChanges(captured).GetProjectChanges()) {
            foreach (var id in project.GetChangedDocuments().Concat(project.GetRemovedDocuments())) {
                var path = captured.GetDocument(id)?.FilePath;
                if (path is null || SourceIds(captured, path).Length != 1)
                    throw new NotSupportedException("Owned refactorings of linked/shared source require all-context propagation");
            }
            if (project.GetAddedDocuments().Any() && SourceIds(captured, document.FilePath!).Length != 1)
                throw new NotSupportedException("Moving a shared source type requires all-context membership updates");
        }
        await ValidateCompilation(captured, changed, token); return await WorkspaceEdit(captured, changed, token, document.Id);
    }, providerCatalog);
    private async Task Publish(Document document, CancellationToken token)
    {
        var text = await document.GetTextAsync(token);
        var model = await document.GetSemanticModelAsync(token) ?? throw new InvalidOperationException("C# semantic model unavailable");
        var diagnostics = model.GetDiagnostics(cancellationToken: token).Where(d => d.Location.IsInSource && d.Location.SourceTree == model.SyntaxTree)
            .Take(10000).Select(d => new { range = Range(text, d.Location.SourceSpan), severity = d.Severity == DiagnosticSeverity.Error ? 1 : d.Severity == DiagnosticSeverity.Warning ? 2 : d.Severity == DiagnosticSeverity.Info ? 3 : 4,
                code = d.Id, source = "Roslyn", message = d.GetMessage(System.Globalization.CultureInfo.InvariantCulture) }).Cast<object>().ToList();
        foreach (var feature in semanticFeatures.OfType<ISemanticContributions>()) diagnostics.AddRange(await feature.Diagnostics(Context(document), token));
        await rpc.Notify("textDocument/publishDiagnostics", new { uri = UriOf(document.FilePath!), version = versions.GetValueOrDefault(document.FilePath!), diagnostics });
    }
    private async Task Update(JsonElement parameters, bool opened, CancellationToken token)
    {
        if (solution is null) throw new InvalidOperationException("Roslyn workspace unavailable");
        var descriptor = parameters.GetProperty("textDocument"); var file = FileOf(descriptor);
        var version = descriptor.GetProperty("version").GetInt32();
        if (!opened && (!versions.TryGetValue(file, out var previous) || version <= previous)) throw new InvalidDataException("Out-of-order managed document version");
        var ids = SourceIds(solution, file);
        if (ids.Length == 0) throw new InvalidOperationException("Editor document is not an evaluated Compile item");
        var document = solution.GetDocument(ids[0])!;
        var text = await document.GetTextAsync(token);
        if (opened) text = SourceText.From(descriptor.GetProperty("text").GetString() ?? "");
        else foreach (var edit in parameters.GetProperty("contentChanges").EnumerateArray()) {
            var replacement = edit.GetProperty("text").GetString() ?? "";
            text = edit.TryGetProperty("range", out var range)
                ? text.WithChanges(new TextChange(TextSpan.FromBounds(Offset(text, range.GetProperty("start")), Offset(text, range.GetProperty("end"))), replacement))
                : SourceText.From(replacement);
        }
        foreach (var id in ids) solution = solution.WithDocumentText(id, text, PreservationMode.PreserveIdentity);
        versions[file] = version;
        // Diagnostics use the same selected semantic context as requests.
        if (versions.Count > 1000) throw new InvalidOperationException("RoslynHost open document budget exceeds 1000 files");
        foreach (var open in versions.Keys.ToArray()) {
            var descriptorOfOpen = JsonSerializer.SerializeToElement(new { textDocument = new { uri = UriOf(open) } });
            await Publish(Document(descriptorOfOpen), token);
        }
    }
    private async Task<object?> Dispatch(JsonElement message, CancellationToken token)
    {
        var method = message.GetProperty("method").GetString();
        var p = message.TryGetProperty("params", out var parameters) ? parameters : default;
        switch (method) {
        case "initialize":
            root = new Uri(p.GetProperty("rootUri").GetString()!).LocalPath;
            await Load(token);
            return new { capabilities = new { positionEncoding = "utf-16", experimental = new { frostLibrarySources = 1 }, textDocumentSync = new { openClose = true, change = 2, save = new { includeText = false } },
                completionProvider = new { triggerCharacters = new[] { "." } }, hoverProvider = true, definitionProvider = true, referencesProvider = true,
                renameProvider = new { prepareProvider = true }, documentFormattingProvider = true, documentSymbolProvider = true, codeLensProvider = new { resolveProvider = false },
                signatureHelpProvider = new { triggerCharacters = new[] { "(", "," } }, codeActionProvider = new { codeActionKinds = new[] { "refactor.rewrite", "refactor.extract", "refactor.inline", "refactor.move" } } }, serverInfo = new { name = "FrostRoslynHost", version = "1" } };
        case "initialized": return null;
        case "shutdown": shutdown = true; return null;
        case "exit": stop.Cancel(); return null;
        case "$/setTrace": case "workspace/didChangeConfiguration": return null;
        case "workspace/didChangeWatchedFiles":
            await Reload(token); return null;
        case "textDocument/didOpen": await Update(p, true, token); return null;
        case "textDocument/didChange": await Update(p, false, token); return null;
        case "textDocument/didSave": await Publish(Document(p), token); return null;
        case "textDocument/didClose":
            var file = FileOf(p.GetProperty("textDocument")); versions.Remove(file);
            if (solution is not null) foreach (var id in SourceIds(solution, file)) solution = solution.WithDocumentText(id, SourceText.From(await File.ReadAllTextAsync(file, token)));
            await rpc.Notify("textDocument/publishDiagnostics", new { uri = UriOf(file), diagnostics = Array.Empty<object>() }); return null;
        case "frost/projectModel":
            if (solution is null) throw new InvalidOperationException("Roslyn workspace unavailable");
            return solution.Projects.Select(project => new { id = project.Id.Id, name = project.Name, projectFile = project.FilePath,
                output = project.OutputFilePath, language = project.Language,
                compile = project.Documents.Select(d => new { path = d.FilePath, folders = d.Folders }).ToArray(),
                references = project.MetadataReferences.Select(r => r.Display).ToArray(), projects = project.ProjectReferences.Select(r => r.ProjectId.Id).ToArray(),
                defines = (project.ParseOptions as Microsoft.CodeAnalysis.CSharp.CSharpParseOptions)?.PreprocessorSymbolNames.ToArray() }).ToArray();
        }
        var doc = Document(p); var source = await doc.GetTextAsync(token);
        var offset = p.TryGetProperty("position", out var point) ? Offset(source, point) : 0;
        if (method is not null) foreach (var feature in semanticFeatures) if (feature.Supports(method)) {
            var captured = doc.Project.Solution;
            var context = new SemanticFeatureContext(doc, async (changed, cancel) => {
                await ValidateSources(captured, cancel); await ValidateCompilation(captured, changed, cancel);
                return await WorkspaceEdit(captured, changed, cancel);
            });
            return await feature.Execute(method, context, p, token);
        }
        switch (method) {
        case "frost/managed/symbol": return await SymbolReceipt(doc, offset, token);
        case "textDocument/completion": return await Complete(doc, source, offset, token);
        case "textDocument/hover":
            var info = await (QuickInfoService.GetService(doc) ?? throw new NotSupportedException("Roslyn QuickInfo unavailable")).GetQuickInfoAsync(doc, offset, token);
            return info is null ? null : new { range = Range(source, info.Span), contents = new { kind = "plaintext", value = string.Join("\n", info.Sections.Select(s => string.Concat(s.TaggedParts.Select(t => t.Text)))) } };
        case "textDocument/definition": return await Definitions(doc, offset, token);
        case "textDocument/references": return await References(doc, offset, p, token);
        case "textDocument/prepareRename":
            var symbol = await SymbolFinder.FindSymbolAtPositionAsync(doc, offset, token);
            if (symbol is null || symbol.Locations.All(l => !l.IsInSource)) throw new NotSupportedException("Only source symbols can be renamed");
            var syntax = await doc.GetSyntaxRootAsync(token); var name = syntax!.FindToken(Math.Min(offset, Math.Max(0, source.Length - 1))).Span;
            return new { range = Range(source, name), placeholder = source.ToString(name) };
        case "textDocument/rename": return await Rename(doc, offset, p.GetProperty("newName").GetString()!, token);
        case "textDocument/formatting": return await Edits(doc, await Formatter.FormatAsync(doc, cancellationToken: token), token);
        case "textDocument/documentSymbol": return await Symbols(doc, source, token);
        case "textDocument/signatureHelp": return await Signature(doc, offset, token);
        case "textDocument/codeAction": return await CodeActions(doc, p, token);
        case "textDocument/codeLens":
            var lenses = new List<object>();
            foreach (var feature in semanticFeatures.OfType<ISemanticContributions>()) lenses.AddRange(await feature.CodeLenses(Context(doc), token));
            return lenses;
        default: throw new NotSupportedException("RoslynHost method unavailable: " + method);
        }
    }
    private async Task Reload(CancellationToken token)
    {
        var overlays = new Dictionary<string, SourceText>(PathComparer);
        if (solution is not null) foreach (var path in versions.Keys) {
            var ids = SourceIds(solution, path);
            if (ids.Length != 0) overlays[path] = await solution.GetDocument(ids[0])!.GetTextAsync(token);
        }
        workspace?.Dispose(); workspace = null; solution = null;
        await Load(token);
        foreach (var overlay in overlays) {
            var ids = SourceIds(solution!, overlay.Key);
            foreach (var id in ids) solution = solution.WithDocumentText(id, overlay.Value, PreservationMode.PreserveIdentity);
            if (ids.Length == 0) await rpc.Notify("textDocument/publishDiagnostics", new { uri = UriOf(overlay.Key), diagnostics = Array.Empty<object>() });
        }
    }
    private static async Task ValidateCompilation(Solution before, Solution after, CancellationToken token)
    {
        foreach (var project in after.Projects) {
            var oldProject = before.GetProject(project.Id);
            if (oldProject is null) throw new NotSupportedException("Refactoring changed a project identity");
            var original = await oldProject.GetCompilationAsync(token);
            var changed = await project.GetCompilationAsync(token);
            if (original is null || changed is null) throw new InvalidOperationException("Roslyn compilation unavailable for refactoring validation");
            var existing = original.GetDiagnostics(token).Where(d => d.Severity == DiagnosticSeverity.Error)
                .GroupBy(d => d.Id + ":" + d.GetMessage(System.Globalization.CultureInfo.InvariantCulture))
                .ToDictionary(group => group.Key, group => group.Count());
            foreach (var error in changed.GetDiagnostics(token).Where(d => d.Severity == DiagnosticSeverity.Error)) {
                var key = error.Id + ":" + error.GetMessage(System.Globalization.CultureInfo.InvariantCulture);
                if (!existing.TryGetValue(key, out var count) || count == 0) throw new InvalidOperationException("Roslyn refactoring introduces compiler error " + key);
                existing[key] = count - 1;
            }
        }
    }    private static async Task<object?> SymbolReceipt(Document doc, int offset, CancellationToken token)
    {
        var symbol = await SymbolFinder.FindSymbolAtPositionAsync(doc, offset, token);
        if (symbol is null) return null;
        symbol = symbol.OriginalDefinition;
        var compilation = await doc.Project.GetCompilationAsync(token) ?? throw new InvalidOperationException("C# compilation unavailable");
        string? assemblyPath = null;
        foreach (var reference in compilation.References.OfType<PortableExecutableReference>())
            if (SymbolEqualityComparer.Default.Equals(compilation.GetAssemblyOrModuleSymbol(reference), symbol.ContainingAssembly)) { assemblyPath = reference.FilePath; break; }
        return new { documentationId = DocumentationCommentId.CreateDeclarationId(symbol), name = symbol.Name,
            display = symbol.ToDisplayString(), assemblyIdentity = symbol.ContainingAssembly?.Identity.ToString(), assemblyPath,
            projectFile = doc.Project.FilePath, metadata = symbol.Locations.Any(location => location.IsInMetadata),
            locations = symbol.Locations.Where(location => location.IsInSource && location.SourceTree is not null)
                .Select(location => new { uri = UriOf(location.SourceTree!.FilePath), range = Range(location.SourceTree!.GetText(token), location.SourceSpan) }).ToArray() };
    }    private async Task<object> Complete(Document doc, SourceText source, int offset, CancellationToken token)
    {
        var service = CompletionService.GetService(doc) ?? throw new NotSupportedException("Roslyn completion unavailable");
        var result = await service.GetCompletionsAsync(doc, offset, cancellationToken: token);
        if (result is null) return new { isIncomplete = false, items = Array.Empty<object>() };
        var items = new List<object>();
        foreach (var item in result.ItemsList.Take(200)) {
            var change = await service.GetChangeAsync(doc, item, cancellationToken: token);
            var description = await service.GetDescriptionAsync(doc, item, cancellationToken: token);
            var tags = item.Tags;
            var kind = tags.Contains("Method") || tags.Contains("ExtensionMethod") ? 2 : tags.Contains("Property") ? 10 : tags.Contains("Class") ? 7 : tags.Contains("Interface") ? 8 : tags.Contains("Keyword") ? 14 : 6;
            items.Add(new { label = item.DisplayTextPrefix + item.DisplayText + item.DisplayTextSuffix, kind,
                sortText = item.SortText, filterText = item.FilterText, detail = description.Text,
                textEdit = new { range = Range(source, change.TextChange.Span), newText = change.TextChange.NewText ?? "" } });
        }
        foreach (var feature in semanticFeatures.OfType<ISemanticContributions>()) items.AddRange(await feature.Completion(Context(doc), offset, token));
        return new { isIncomplete = result.ItemsList.Count > 200, items };
    }
    private static async Task<object[]> Definitions(Document doc, int offset, CancellationToken token)
    {
        var symbol = await SymbolFinder.FindSymbolAtPositionAsync(doc, offset, token);
        if (symbol is null) return Array.Empty<object>();
        symbol = await SymbolFinder.FindSourceDefinitionAsync(symbol, doc.Project.Solution, token) ?? symbol;
        // Metadata remains explicitly unavailable until LTR-6 supplies its read-only source cache.
        return symbol.Locations.Where(l => l.IsInSource && l.SourceTree is not null)
            .Select(l => (object)new { uri = UriOf(l.SourceTree!.FilePath), range = Range(l.SourceTree!.GetText(token), l.SourceSpan) }).ToArray();
    }
    private static async Task<object[]> References(Document doc, int offset, JsonElement p, CancellationToken token)
    {
        var symbol = await SymbolFinder.FindSymbolAtPositionAsync(doc, offset, token);
        if (symbol is null) return Array.Empty<object>();
        var found = await SymbolFinder.FindReferencesAsync(symbol, doc.Project.Solution, cancellationToken: token);
        var locations = found.SelectMany(r => r.Locations.Select(l => l.Location)).ToList();
        if (p.TryGetProperty("context", out var context) && context.TryGetProperty("includeDeclaration", out var include) && include.GetBoolean()) locations.AddRange(symbol.Locations.Where(l => l.IsInSource));
        if (locations.Count > 10000) throw new InvalidOperationException("References exceed the 10000-location response budget");
        return locations.Where(l => l.SourceTree is not null).Select(l => (object)new { uri = UriOf(l.SourceTree!.FilePath), range = Range(l.SourceTree!.GetText(token), l.SourceSpan) }).ToArray();
    }
    private static async Task<object[]> Edits(Document before, Document after, CancellationToken token)
    {
        var text = await before.GetTextAsync(token);
        return (await after.GetTextChangesAsync(before, token)).Select(c => (object)new { range = Range(text, c.Span), newText = c.NewText ?? "" }).ToArray();
    }
    private async Task ValidateSources(Solution captured, CancellationToken token)
    {
        long bytes = 0; int files = 0;
        foreach (var doc in captured.Projects.SelectMany(p => p.Documents)) {
            if (++files > 10000) throw new InvalidOperationException("Managed semantic source budget exceeds 10000 files");
            if (doc.FilePath is null) throw new InvalidOperationException("Managed document has no physical source identity");
            var text = await doc.GetTextAsync(token); bytes += text.Length * 2L;
            if (bytes > 64L * 1024 * 1024) throw new InvalidOperationException("Managed semantic source budget exceeds 64 MiB");
            if (!versions.ContainsKey(doc.FilePath) && await File.ReadAllTextAsync(doc.FilePath, token) != text.ToString())
                throw new InvalidOperationException("Managed source changed outside this session; reload the evaluated workspace before refactoring");
        }
    }
    private async Task<object> WorkspaceEdit(Solution before, Solution after, CancellationToken token, DocumentId? origin = null)
    {
        var delta = after.GetChanges(before);
        if (delta.GetAddedProjects().Any() || delta.GetRemovedProjects().Any()) throw new NotSupportedException("Project identity changes cannot enter REF1");
        var changes = new Dictionary<string, object[]>(PathComparer);
        var inputs = new Dictionary<string, string>(PathComparer);
        var creates = new List<object>(); var renames = new List<object>();
        var createdPaths = new List<string>(); var movedPaths = new List<(string From, string To)>();
        foreach (var document in before.Projects.SelectMany(p => p.Documents)) {
            token.ThrowIfCancellationRequested();
            if (document.FilePath is null) throw new NotSupportedException("Source without physical path");
            var image = (await document.GetTextAsync(token)).ToString();
            if (inputs.TryGetValue(document.FilePath, out var linked) && linked != image) throw new InvalidOperationException("Linked source images disagree");
            inputs[document.FilePath] = image;
        }
        foreach (var project in before.Projects) {
            if (project.FilePath is null) throw new NotSupportedException("Project without physical path");
            inputs[project.FilePath] = await File.ReadAllTextAsync(project.FilePath, token);
        }
        if (inputs.Count > 1000 || inputs.Values.Sum(v => (long)System.Text.Encoding.UTF8.GetByteCount(v)) > 4 * 1024 * 1024)
            throw new NotSupportedException("Managed source receipt budget exceeded");
        var selected = origin is null ? null : before.GetDocument(origin);
        Project? resourceProject = null;
        foreach (var project in delta.GetProjectChanges()) {
            if (project.GetRemovedDocuments().Any() || project.GetAddedAdditionalDocuments().Any() || project.GetRemovedAdditionalDocuments().Any() ||
                project.GetChangedAdditionalDocuments().Any() || project.GetAddedAnalyzerConfigDocuments().Any() || project.GetRemovedAnalyzerConfigDocuments().Any() || project.GetChangedAnalyzerConfigDocuments().Any())
                throw new NotSupportedException("Unsupported non-source or deleted document delta");
            foreach (var id in project.GetAddedDocuments()) {
                var added = after.GetDocument(id)!;
                if (selected is null || added.Project.Id != selected.Project.Id || added.FilePath is null || selected.FilePath is null ||
                    !PathComparer.Equals(Path.GetDirectoryName(added.FilePath), Path.GetDirectoryName(selected.FilePath)) ||
                    !added.FilePath.EndsWith(".cs", StringComparison.OrdinalIgnoreCase) || File.Exists(added.FilePath) || Directory.Exists(added.FilePath))
                    throw new NotSupportedException("Move type requires an absent same-directory C# endpoint in the selected project");
                creates.Add(new { uri = UriOf(added.FilePath), text = (await added.GetTextAsync(token)).ToString() });
                createdPaths.Add(added.FilePath); resourceProject = selected.Project;
            }
            foreach (var id in project.GetChangedDocuments()) {
                var oldDoc = before.GetDocument(id)!; var newDoc = after.GetDocument(id)!;
                var syntax = await newDoc.GetSyntaxRootAsync(token);
                if (syntax is not null && syntax.GetAnnotatedNodesAndTokens(Microsoft.CodeAnalysis.CodeActions.ConflictAnnotation.Kind).Any())
                    throw new InvalidOperationException("Roslyn reports an unresolved refactoring conflict");
                if (oldDoc.FilePath is null || newDoc.FilePath is null) throw new NotSupportedException("Source without physical path");
                if (oldDoc.FilePath != newDoc.FilePath) {
                    if (selected is null || id != selected.Id || PathComparer.Equals(oldDoc.FilePath, newDoc.FilePath) ||
                        !PathComparer.Equals(Path.GetDirectoryName(oldDoc.FilePath), Path.GetDirectoryName(newDoc.FilePath)) ||
                        !newDoc.FilePath.EndsWith(".cs", StringComparison.OrdinalIgnoreCase) || File.Exists(newDoc.FilePath) || Directory.Exists(newDoc.FilePath) ||
                        !(await oldDoc.GetTextAsync(token)).ContentEquals(await newDoc.GetTextAsync(token)))
                        throw new NotSupportedException("File rename requires unchanged bytes and distinct absent same-directory endpoint; case-only moves are refused");
                    renames.Add(new { oldUri = UriOf(oldDoc.FilePath), newUri = UriOf(newDoc.FilePath) });
                    movedPaths.Add((oldDoc.FilePath, newDoc.FilePath)); resourceProject = selected.Project;
                    continue;
                }
                var edits = await Edits(oldDoc, newDoc, token);
                if (changes.TryGetValue(oldDoc.FilePath, out var linked) && JsonSerializer.Serialize(linked) != JsonSerializer.Serialize(edits))
                    throw new InvalidOperationException("Linked project contexts propose inconsistent source edits");
                changes[oldDoc.FilePath] = edits;
            }
        }
        if (creates.Count + renames.Count > 1) throw new NotSupportedException("Only one move/create endpoint is supported per semantic proposal");
        if (resourceProject is not null) {
            var path = resourceProject.FilePath!;
            var xml = XDocument.Parse(inputs[path], LoadOptions.PreserveWhitespace);
            var element = xml.Root ?? throw new InvalidOperationException("Missing project XML root");
            if ((string?)element.Attribute("Sdk") != "Microsoft.NET.Sdk" ||
                element.DescendantsAndSelf().Any(e => e.Attributes().Any(a => a.Name.LocalName == "Condition")) ||
                element.Descendants().Any(e => e.Name.LocalName is "Import" or "Target" ||
                    (e.Name.LocalName == "Compile" && (e.HasElements || e.Attributes().Any(a => a.Name.LocalName is not ("Include" or "Remove"))))))
                throw new NotSupportedException("Move type/file requires a simple unconditional Microsoft.NET.Sdk project without custom imports/targets or Compile metadata");
            for (var directory = new DirectoryInfo(Path.GetDirectoryName(path)!); directory is not null; directory = directory.Parent)
                foreach (var name in new[] { "Directory.Build.props", "Directory.Build.targets" })
                    if (File.Exists(Path.Combine(directory.FullName, name))) throw new NotSupportedException("Move type/file with inherited project overrides is unavailable");
            var ns = element.Name.Namespace;
            var group = new XElement(ns + "ItemGroup");
            foreach (var move in movedPaths) group.Add(new XElement(ns + "Compile", new XAttribute("Remove", Path.GetRelativePath(Path.GetDirectoryName(path)!, move.From))));
            foreach (var target in createdPaths.Concat(movedPaths.Select(m => m.To))) {
                var relative = Path.GetRelativePath(Path.GetDirectoryName(path)!, target);
                if (relative.Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar).Contains("..")) throw new NotSupportedException("Move endpoint outside project");
                group.Add(new XElement(ns + "Compile", new XAttribute("Remove", relative)), new XElement(ns + "Compile", new XAttribute("Include", relative)));
            }
            element.Add(group);
            var projectText = SourceText.From(inputs[path]);
            changes[path] = [new { range = Range(projectText, new TextSpan(0, projectText.Length)), newText = xml.ToString(SaveOptions.DisableFormatting) }];
        }
        var documentChanges = changes.Select(change => new { textDocument = new { uri = UriOf(change.Key), version = versions.TryGetValue(change.Key, out var version) ? (int?)version : null }, edits = change.Value }).ToArray();
        if (origin is null) return new { documentChanges }; // Existing rename/simplify callers keep their contract.
        return new { documentChanges,
            frostManaged = new { producer = "FrostRoslynHost", inputs = inputs.Select(input => new { uri = UriOf(input.Key), text = input.Value }).ToArray(), creates, renames } };
    }
    private async Task<object> Rename(Document doc, int offset, string newName, CancellationToken token)
    {
        if (!Microsoft.CodeAnalysis.CSharp.SyntaxFacts.IsValidIdentifier(newName)) throw new ArgumentException("Rename requires a valid C# identifier");
        var symbol = await SymbolFinder.FindSymbolAtPositionAsync(doc, offset, token) ?? throw new InvalidOperationException("No Roslyn symbol at rename position");
        if (symbol.Locations.All(l => !l.IsInSource) || symbol.IsImplicitlyDeclared) throw new NotSupportedException("Only explicitly declared source symbols can be renamed");
        var captured = doc.Project.Solution;
        await ValidateSources(captured, token);
        var changed = await Renamer.RenameSymbolAsync(captured, symbol, new SymbolRenameOptions(), newName, token);
        // Return a proposal only. The C++ REF1 owner reviews/adopts it; Roslyn never writes files.
        await ValidateCompilation(captured, changed, token);
        return await WorkspaceEdit(captured, changed, token);
    }
    private static async Task<object[]> Symbols(Document doc, SourceText text, CancellationToken token)
    {
        var syntax = await doc.GetSyntaxRootAsync(token); var model = await doc.GetSemanticModelAsync(token);
        if (syntax is null || model is null) return Array.Empty<object>();
        var result = new List<object>();
        foreach (var node in syntax.DescendantNodes().Where(n => n is MemberDeclarationSyntax || n is VariableDeclaratorSyntax)) {
            var symbol = model.GetDeclaredSymbol(node, token);
            if (symbol is null || symbol.IsImplicitlyDeclared) continue;
            if (result.Count == 10000) throw new InvalidOperationException("Managed symbol response exceeds 10000 declarations");
            var kind = symbol.Kind == SymbolKind.NamedType ? 5 : symbol.Kind == SymbolKind.Method ? 6 : symbol.Kind == SymbolKind.Property ? 7 : symbol.Kind == SymbolKind.Namespace ? 3 : 8;
            result.Add(new { name = symbol.Name, kind, containerName = symbol.ContainingSymbol?.ToDisplayString(), location = new { uri = UriOf(doc.FilePath!), range = Range(text, node.Span) } });
        }
        return result.ToArray();
    }
    private static async Task<object?> Signature(Document doc, int offset, CancellationToken token)
    {
        var syntax = await doc.GetSyntaxRootAsync(token); var model = await doc.GetSemanticModelAsync(token);
        if (syntax is null || model is null) return null;
        var invocation = syntax.FindToken(Math.Min(offset, Math.Max(0, syntax.FullSpan.End - 1))).Parent?.AncestorsAndSelf().OfType<InvocationExpressionSyntax>().FirstOrDefault();
        if (invocation is null) return null;
        var info = model.GetSymbolInfo(invocation, token);
        var methods = (info.Symbol is IMethodSymbol method ? new[] { method } : info.CandidateSymbols.OfType<IMethodSymbol>()).ToArray();
        if (methods.Length == 0) return null;
        var active = invocation.ArgumentList.Arguments.GetSeparators().Count(separator => separator.SpanStart < offset);
        return new { signatures = methods.Select(m => new { label = m.ToDisplayString(SymbolDisplayFormat.MinimallyQualifiedFormat), parameters = m.Parameters.Select(parameter => new { label = parameter.ToDisplayString(SymbolDisplayFormat.MinimallyQualifiedFormat) }).ToArray() }).ToArray(), activeSignature = 0, activeParameter = active };
    }
    private async Task<object[]> CodeActions(Document doc, JsonElement parameters, CancellationToken token)
    {
        var captured = doc.Project.Solution;
        await ValidateSources(captured, token);
        var root = await doc.GetSyntaxRootAsync(token) ?? throw new InvalidOperationException("C# syntax unavailable");
        var simplified = await Simplifier.ReduceAsync(doc.WithSyntaxRoot(root.WithAdditionalAnnotations(Simplifier.Annotation)), cancellationToken: token);
        simplified = await Formatter.FormatAsync(simplified, cancellationToken: token);
        await ValidateCompilation(captured, simplified.Project.Solution, token);
        var edit = await WorkspaceEdit(captured, simplified.Project.Solution, token);
        var actions = new List<object> { new { title = "Simplify C# names and format (Roslyn)", kind = "refactor.rewrite", edit } };
        foreach (var feature in semanticFeatures.OfType<ISemanticContributions>()) actions.AddRange(await feature.CodeActions(Context(doc), parameters, token));
        return actions.ToArray();
    }
}
