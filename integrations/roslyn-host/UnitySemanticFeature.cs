// SPDX-License-Identifier: Apache-2.0
// Source-backed Unity semantics. Unity's public message contract, never token-spelling inference.
using System.Text.Json;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Formatting;
using Microsoft.CodeAnalysis.Text;
namespace Frost.RoslynHost;
internal sealed class UnitySemanticFeature : ISemanticFeature, ISemanticContributions
{
    private static readonly HashSet<string> Events = new(StringComparer.Ordinal) {
        "Awake", "Start", "Update", "FixedUpdate", "LateUpdate", "OnEnable", "OnDisable", "OnDestroy", "Reset", "OnValidate", "OnApplicationQuit"
    };
    private static readonly HashSet<string> PerFrame = new(StringComparer.Ordinal) { "Update", "FixedUpdate", "LateUpdate" };
    public bool Supports(string method) => method is "frost/unity/semantics" or "frost/unity/generateEvent";
    private static object Identity(ISymbol symbol, Project project) => new {
        declarationId = DocumentationCommentId.CreateDeclarationId(symbol),
        assembly = symbol.ContainingAssembly?.Identity.ToString(), project = project.Id.Id.ToString(),
        projectPath = project.FilePath, symbol = symbol.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat)
    };
    private static bool Derives(INamedTypeSymbol? type, INamedTypeSymbol? expected) {
        if (expected is null) return false;
        for (var current = type; current is not null; current = current.BaseType)
            if (SymbolEqualityComparer.Default.Equals(current, expected)) return true;
        return false;
    }
    private static bool Attribute(ISymbol symbol, INamedTypeSymbol? attribute) => attribute is not null &&
        symbol.GetAttributes().Any(value => SymbolEqualityComparer.Default.Equals(value.AttributeClass, attribute));
    private static object Range(SyntaxNode node) {
        var span = node.GetLocation().GetLineSpan().Span;
        return new { start = new { line = span.Start.Line, character = span.Start.Character }, end = new { line = span.End.Line, character = span.End.Character } };
    }
    private static bool EventSignature(IMethodSymbol method, Compilation compilation) => !method.IsStatic && !method.IsAbstract &&
        !method.IsGenericMethod && method.MethodKind == MethodKind.Ordinary && method.Parameters.Length == 0 &&
        (method.ReturnsVoid || (method.Name == "Start" && SymbolEqualityComparer.Default.Equals(method.ReturnType, compilation.GetTypeByMetadataName("System.Collections.IEnumerator"))));
    private static bool SerializableType(ITypeSymbol type, Compilation compilation, int depth = 0) {
        if (depth > 0 && (type is IArrayTypeSymbol || (type is INamedTypeSymbol container && SymbolEqualityComparer.Default.Equals(container.OriginalDefinition, compilation.GetTypeByMetadataName("System.Collections.Generic.List`1"))))) return false;
        if (depth > 8 || type.TypeKind is TypeKind.Error or TypeKind.Pointer or TypeKind.TypeParameter) return false;
        if (type.SpecialType is SpecialType.System_String or SpecialType.System_Boolean or SpecialType.System_Char or
            SpecialType.System_Byte or SpecialType.System_SByte or SpecialType.System_Int16 or SpecialType.System_UInt16 or
            SpecialType.System_Int32 or SpecialType.System_UInt32 or SpecialType.System_Int64 or SpecialType.System_UInt64 or
            SpecialType.System_Single or SpecialType.System_Double || type.TypeKind == TypeKind.Enum) return true;
        if (type is IArrayTypeSymbol array) return array.Rank == 1 && array.ElementType is not IArrayTypeSymbol && SerializableType(array.ElementType, compilation, depth + 1);
        if (type is not INamedTypeSymbol named) return false;
        if (SymbolEqualityComparer.Default.Equals(named.OriginalDefinition, compilation.GetTypeByMetadataName("System.Collections.Generic.List`1")))
            return named.TypeArguments.Length == 1 && named.TypeArguments[0] is not IArrayTypeSymbol && SerializableType(named.TypeArguments[0], compilation, depth + 1);
        return Derives(named, compilation.GetTypeByMetadataName("UnityEngine.Object")) ||
            (!named.IsGenericType && Attribute(named, compilation.GetTypeByMetadataName("System.SerializableAttribute")));
    }
    public async Task<object?> Execute(string method, SemanticFeatureContext context, JsonElement parameters, CancellationToken token) {
        var document = context.Document;
        var root = await document.GetSyntaxRootAsync(token) ?? throw new InvalidOperationException("C# syntax unavailable");
        var model = await document.GetSemanticModelAsync(token) ?? throw new InvalidOperationException("C# semantics unavailable");
        var compilation = model.Compilation;
        var mono = compilation.GetTypeByMetadataName("UnityEngine.MonoBehaviour");
        var scriptable = compilation.GetTypeByMetadataName("UnityEngine.ScriptableObject");
        if (mono is null && scriptable is null) return new { available = false, reason = "Evaluated compilation has no Unity types" };
        var source = await document.GetTextAsync(token);
        var position = parameters.GetProperty("position");
        var line = position.GetProperty("line").GetInt32(); var character = position.GetProperty("character").GetInt32();
        if (line < 0 || line >= source.Lines.Count || character < 0 || character > source.Lines[line].Span.Length) throw new ArgumentException("Unity semantic position is outside captured source");
        var offset = source.Lines[line].Start + character;
        var tokenNode = root.FindToken(Math.Min(offset, Math.Max(0, root.FullSpan.End - 1))).Parent;
        ISymbol? selectedSymbol = tokenNode is null ? null : model.GetSymbolInfo(tokenNode, token).Symbol;
        if (selectedSymbol is null && tokenNode is VariableDeclaratorSyntax variable) selectedSymbol = model.GetDeclaredSymbol(variable, token);
        if (selectedSymbol is null && tokenNode is MethodDeclarationSyntax methodDeclaration) selectedSymbol = model.GetDeclaredSymbol(methodDeclaration, token);
        object? selectedMember = null;
        var selected = root.FindToken(Math.Min(offset, Math.Max(0, root.FullSpan.End - 1))).Parent?.AncestorsAndSelf().OfType<TypeDeclarationSyntax>().FirstOrDefault();
        if (parameters.TryGetProperty("allTypes", out var allTypes) && allTypes.ValueKind == JsonValueKind.True) selected = null;
        var declarations = selected is null ? root.DescendantNodes().OfType<TypeDeclarationSyntax>() : new[] { selected };
        var types = new List<object>();
        foreach (var declaration in declarations) {
            token.ThrowIfCancellationRequested();
            if (model.GetDeclaredSymbol(declaration, token) is not INamedTypeSymbol type) continue;
            var isMono = Derives(type, mono); var isScriptable = Derives(type, scriptable);
            if (!isMono && !isScriptable) continue;
            if (SymbolEqualityComparer.Default.Equals(selectedSymbol, type)) selectedMember = new { kind = "asset", name = type.Name, identity = Identity(type, document.Project), sourcePath = declaration.SyntaxTree.FilePath, declaringType = type.Name };
            if (method == "frost/unity/generateEvent") {
                var name = parameters.GetProperty("eventName").GetString() ?? "";
                if (!Events.Contains(name) || (!isMono && name is not ("OnEnable" or "OnDisable" or "OnDestroy" or "OnValidate"))) throw new ArgumentException("Unsupported Unity event for selected type");
                if (type.GetMembers(name).Length != 0) throw new InvalidOperationException("Selected type already declares this event name; no duplicate or signature rewrite is proposed");
                var member = SyntaxFactory.ParseMemberDeclaration("private void " + name + "() { }") ?? throw new InvalidOperationException("Cannot create Unity event");
                var changedDeclaration = declaration.AddMembers(member.WithAdditionalAnnotations(Formatter.Annotation));
                var changed = document.WithSyntaxRoot(root.ReplaceNode(declaration, changedDeclaration));
                changed = await Formatter.FormatAsync(changed, Formatter.Annotation, cancellationToken: token);
                return await context.CreateWorkspaceEditAsync(changed.Project.Solution, token);
            }
            var implicitUses = new List<object>(); var inspections = new List<object>(); var fields = new List<object>(); var eventMethods = new List<object>(); var hotspots = new List<object>();
            foreach (var member in type.GetMembers()) {
                var syntaxReference = member.DeclaringSyntaxReferences.FirstOrDefault(value => value.SyntaxTree == root.SyntaxTree);
                if (syntaxReference is null) continue;
                var memberRange = Range(await syntaxReference.GetSyntaxAsync(token));
                if (member is IMethodSymbol function && Events.Contains(function.Name) &&
                    (isMono || function.Name is "OnEnable" or "OnDisable" or "OnDestroy" or "OnValidate")) {
                    if (EventSignature(function, compilation)) implicitUses.Add(new { identity = Identity(function, document.Project), range = memberRange, reason = "Unity event function", performanceCritical = PerFrame.Contains(function.Name) });
                    else inspections.Add(new { code = "FUNITY001", range = memberRange, identity = Identity(function, document.Project), message = "Unity event signature is not supported by this event contract; expected a non-static parameterless void method (Start may return IEnumerator)", severity = "warning" });
                }
                if (member is IFieldSymbol field && !field.IsStatic && !field.IsConst && !field.IsReadOnly &&
                    !Attribute(field, compilation.GetTypeByMetadataName("System.NonSerializedAttribute"))) {
                    var serialize = Attribute(field, compilation.GetTypeByMetadataName("UnityEngine.SerializeField"));
                    var reference = Attribute(field, compilation.GetTypeByMetadataName("UnityEngine.SerializeReference"));
                    if (serialize || reference || field.DeclaredAccessibility == Accessibility.Public) {
                        var supported = SerializableType(field.Type, compilation);
                        if (supported && !reference) {
                            if (SymbolEqualityComparer.Default.Equals(selectedSymbol, field)) selectedMember = new { kind = "field", name = field.Name, identity = Identity(field, document.Project), sourcePath = syntaxReference.SyntaxTree.FilePath, declaringType = type.Name };
                            fields.Add(new { identity = Identity(field, document.Project), name = field.Name, range = memberRange, type = field.Type.ToDisplayString(), declaringType = Identity(type, document.Project), serialized = true });
                        }
                        else if (serialize && !reference) inspections.Add(new { code = "FUNITY002", range = memberRange, identity = Identity(field, document.Project), message = "SerializeField type is outside the supported Unity serialization contract", severity = "information" });
                    }
                }
                if (member is IMethodSymbol candidate && candidate.MethodKind == MethodKind.Ordinary && !candidate.IsStatic && !candidate.IsGenericMethod && candidate.ReturnsVoid && candidate.DeclaredAccessibility == Accessibility.Public)
                {
                    if (SymbolEqualityComparer.Default.Equals(selectedSymbol, candidate) && candidate.Parameters.Length == 0 && type.GetMembers(candidate.Name).OfType<IMethodSymbol>().Count() == 1)
                        selectedMember = new { kind = "eventMethod", name = candidate.Name, identity = Identity(candidate, document.Project), sourcePath = syntaxReference.SyntaxTree.FilePath, declaringType = type.Name };
                    eventMethods.Add(new { identity = Identity(candidate, document.Project), name = candidate.Name, declaringType = Identity(type, document.Project), parameters = candidate.Parameters.Select(p => p.Type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat)).ToArray() });
                }
            }
            foreach (var invocation in declaration.DescendantNodes().OfType<InvocationExpressionSyntax>()) {
                var ownerSyntax = invocation.Ancestors().OfType<MethodDeclarationSyntax>().FirstOrDefault();
                if (invocation.Ancestors().TakeWhile(node => node != ownerSyntax).Any(node => node is AnonymousFunctionExpressionSyntax or LocalFunctionStatementSyntax)) continue;
                if (ownerSyntax is null || model.GetDeclaredSymbol(ownerSyntax, token) is not IMethodSymbol caller || !SymbolEqualityComparer.Default.Equals(caller.ContainingType, type) || !PerFrame.Contains(caller.Name) || !EventSignature(caller, compilation)) continue;
                if (model.GetSymbolInfo(invocation, token).Symbol is not IMethodSymbol called) continue;
                var gameObject = compilation.GetTypeByMetadataName("UnityEngine.GameObject"); var unityObject = compilation.GetTypeByMetadataName("UnityEngine.Object");
                if ((SymbolEqualityComparer.Default.Equals(called.ContainingType, gameObject) && called.Name is "Find" or "FindWithTag" or "FindGameObjectsWithTag" or "GetComponent") ||
                    (SymbolEqualityComparer.Default.Equals(called.ContainingType, unityObject) && called.Name is "FindObjectOfType" or "FindObjectsOfType" or "FindAnyObjectByType" or "FindFirstObjectByType"))
                    hotspots.Add(new { code = "FUNITY003", range = Range(invocation), identity = Identity(called, document.Project), context = Identity(caller, document.Project), message = "Unity object lookup in a per-frame event; consider caching when lifecycle semantics permit" });
            }
            var completions = Events.Where(name => (isMono || name is "OnEnable" or "OnDisable" or "OnDestroy" or "OnValidate") && type.GetMembers(name).Length == 0)
                .OrderBy(name => name, StringComparer.Ordinal).Select(name => new { label = name, signature = "void " + name + "()", method = "frost/unity/generateEvent", eventName = name });
            types.Add(new { identity = Identity(type, document.Project), range = Range(declaration), implicitUses, inspections, fields, eventMethods, hotspots, completions });
        }
        return new { available = true, contract = "unity-source-semantics-v1", project = document.Project.Id.Id.ToString(), document = document.FilePath, selectedMember, types,
            limitations = new[] { "Bounded event and serialization contract; unresolved types are excluded", "UnityEvent overload and serialized asset identity require original YAML object evidence", "SerializeReference polymorphism is not guessed" } };
    }
    private static JsonElement Parameters(SourceText source, int offset) {
        var at = source.Lines.GetLinePosition(Math.Clamp(offset, 0, source.Length));
        return JsonSerializer.SerializeToElement(new { position = new { line = at.Line, character = at.Character }, allTypes = offset == 0 });
    }
    private async Task<JsonElement> Facts(SemanticFeatureContext context, JsonElement parameters, CancellationToken token) =>
        JsonSerializer.SerializeToElement(await Execute("frost/unity/semantics", context, parameters, token));
    public async Task<IReadOnlyList<object>> Completion(SemanticFeatureContext context, int offset, CancellationToken token) {
        var root = await context.Document.GetSyntaxRootAsync(token);
        if (root is null || root.FindToken(Math.Clamp(offset, 0, Math.Max(0, root.FullSpan.End - 1))).Parent?.AncestorsAndSelf().Any(node => node is MethodDeclarationSyntax or PropertyDeclarationSyntax or FieldDeclarationSyntax) == true) return Array.Empty<object>();
        var facts = await Facts(context, Parameters(await context.Document.GetTextAsync(token), offset), token);
        var result = new List<object>();
        if (!facts.TryGetProperty("types", out var types)) return result;
        foreach (var type in types.EnumerateArray()) foreach (var item in type.GetProperty("completions").EnumerateArray()) {
            var name = item.GetProperty("label").GetString()!;
            result.Add(new { label = name, kind = 2, detail = "Unity event function", insertTextFormat = 2,
                insertText = "private void " + name + "()\n{\n\t$0\n}", sortText = "0Unity_" + name });
        }
        return result;
    }
    public async Task<IReadOnlyList<object>> Diagnostics(SemanticFeatureContext context, CancellationToken token) {
        var facts = await Facts(context, Parameters(await context.Document.GetTextAsync(token), 0), token);
        var result = new List<object>();
        if (!facts.TryGetProperty("types", out var types)) return result;
        foreach (var type in types.EnumerateArray()) {
            foreach (var issue in type.GetProperty("inspections").EnumerateArray()) result.Add(new {
                range = issue.GetProperty("range"), code = issue.GetProperty("code").GetString(), source = "FrostIDE Unity",
                severity = issue.GetProperty("severity").GetString() == "warning" ? 2 : 3, message = issue.GetProperty("message").GetString() });
            foreach (var issue in type.GetProperty("hotspots").EnumerateArray()) result.Add(new {
                range = issue.GetProperty("range"), code = "FUNITY003", source = "FrostIDE Unity", severity = 3,
                message = issue.GetProperty("message").GetString(), tags = Array.Empty<int>() });
        }
        return result;
    }
    public async Task<IReadOnlyList<object>> CodeActions(SemanticFeatureContext context, JsonElement parameters, CancellationToken token) {
        var range = parameters.GetProperty("range").GetProperty("start");
        var selected = JsonSerializer.SerializeToElement(new { position = range });
        var facts = await Facts(context, selected, token);
        var result = new List<object>();
        if (!facts.TryGetProperty("types", out var types)) return result;
        // Keep menu preparation bounded. The complete supported catalog remains in completion.
        foreach (var type in types.EnumerateArray()) foreach (var item in type.GetProperty("completions").EnumerateArray().Take(2)) {
            var name = item.GetProperty("label").GetString()!;
            var request = JsonSerializer.SerializeToElement(new { position = range, eventName = name });
            var edit = await Execute("frost/unity/generateEvent", context, request, token);
            result.Add(new { title = "Generate Unity " + name, kind = "refactor.rewrite", edit });
        }
        return result;
    }
    public async Task<IReadOnlyList<object>> CodeLenses(SemanticFeatureContext context, CancellationToken token) {
        var facts = await Facts(context, Parameters(await context.Document.GetTextAsync(token), 0), token);
        var result = new List<object>();
        if (!facts.TryGetProperty("types", out var types)) return result;
        foreach (var type in types.EnumerateArray()) {
            foreach (var callback in type.GetProperty("implicitUses").EnumerateArray()) result.Add(new {
                range = callback.GetProperty("range"), command = new { title = callback.GetProperty("performanceCritical").GetBoolean() ? "Unity per-frame event" : "Unity event function", command = "navigate.findUsages" } });
            foreach (var field in type.GetProperty("fields").EnumerateArray()) result.Add(new {
                range = field.GetProperty("range"), command = new { title = "Unity serialized field", command = "navigate.findUsages" }, data = new { identity = field.GetProperty("identity"), field = field.GetProperty("name").GetString(), sourcePath = context.Document.FilePath } });
        }
        return result;
    }
}
