// SPDX-License-Identifier: Apache-2.0
// Bounded Roslyn signature reorder preserves argument evaluation order and rechecks bound identities.
using System.Text.Json;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.FindSymbols;
using Microsoft.CodeAnalysis.Formatting;
using Microsoft.CodeAnalysis.Operations;
namespace Frost.RoslynHost;
internal sealed class SignatureRefactorings : ISemanticFeature, ISemanticContributions
{
    public bool Supports(string method) => false;
    public Task<object?> Execute(string method, SemanticFeatureContext context, JsonElement parameters, CancellationToken token) => throw new NotSupportedException(method);
    public async Task<IReadOnlyList<object>> CodeActions(SemanticFeatureContext context, JsonElement parameters, CancellationToken token)
    {
        var document = context.Document;
        var selection = await ProviderRefactorings.Selection(document, parameters, token);
        var root = await document.GetSyntaxRootAsync(token);
        var model = await document.GetSemanticModelAsync(token);
        if (root is null || model is null || root.FullSpan.IsEmpty) return [];
        var node = root.FindToken(Math.Min(selection.Start, root.FullSpan.End - 1)).Parent;
        var declaration = node?.AncestorsAndSelf().OfType<MethodDeclarationSyntax>().FirstOrDefault();
        if (declaration is null || !declaration.ParameterList.FullSpan.Contains(selection) || declaration.ContainsDirectives ||
            model.GetDeclaredSymbol(declaration, token) is not IMethodSymbol method || method.DeclaredAccessibility != Accessibility.Private ||
            !method.IsStatic || method.MethodKind != MethodKind.Ordinary || method.IsGenericMethod || method.IsVirtual || method.IsOverride ||
            method.IsAbstract || method.IsExtern || method.PartialDefinitionPart is not null || method.PartialImplementationPart is not null ||
            method.DeclaringSyntaxReferences.Length != 1 || method.GetAttributes().Length != 0 || method.Parameters.Length < 2 ||
            method.Parameters.Any(p => p.IsOptional || p.IsParams || p.RefKind != RefKind.None || p.GetAttributes().Length != 0)) return [];
        // Reordering can otherwise steal a call from a previously unrelated overload.
        for (var type = method.ContainingType; type is not null; type = type.BaseType)
            if (type.GetMembers(method.Name).OfType<IMethodSymbol>().Any(other => !SymbolEqualityComparer.Default.Equals(other.OriginalDefinition, method.OriginalDefinition))) return [];
        var before = document.Project.Solution;
        var replacements = new Dictionary<DocumentId, Dictionary<SyntaxNode, SyntaxNode>>();
        void Add(DocumentId id, SyntaxNode oldNode, SyntaxNode newNode) {
            if (!replacements.TryGetValue(id, out var edits)) replacements[id] = edits = new();
            edits[oldNode] = newNode;
        }
        var target = new SyntaxAnnotation("Frost.Signature.Target");
        var parametersList = declaration.ParameterList.Parameters;
        var reordered = SyntaxFactory.SeparatedList(parametersList.Select((p, index) => index == 0 ? parametersList[1] : index == 1 ? parametersList[0] : p));
        Add(document.Id, declaration, declaration.WithParameterList(declaration.ParameterList.WithParameters(reordered)).WithAdditionalAnnotations(target, Formatter.Annotation));
        var uses = new List<(DocumentId Id, SyntaxAnnotation Mark)>();
        foreach (var referenced in await SymbolFinder.FindReferencesAsync(method, before, cancellationToken: token)) {
            if (!SymbolEqualityComparer.Default.Equals(referenced.Definition.OriginalDefinition, method.OriginalDefinition)) return [];
            foreach (var location in referenced.Locations) {
                var callDocument = location.Document;
                if (location.IsImplicit || callDocument.Project.Id != document.Project.Id) return [];
                var callRoot = await callDocument.GetSyntaxRootAsync(token);
                var callModel = await callDocument.GetSemanticModelAsync(token);
                var invocation = callRoot?.FindNode(location.Location.SourceSpan).AncestorsAndSelf().OfType<InvocationExpressionSyntax>().FirstOrDefault();
                if (invocation is null || invocation.ContainsDirectives || callModel?.GetOperation(invocation, token) is not IInvocationOperation operation ||
                    !SymbolEqualityComparer.Default.Equals(operation.TargetMethod.OriginalDefinition, method.OriginalDefinition) ||
                    operation.Arguments.Length != method.Parameters.Length || operation.Arguments.Any(a => a.IsImplicit || a.ArgumentKind != ArgumentKind.Explicit)) return [];
                var named = new Dictionary<ArgumentSyntax, ArgumentSyntax>();
                foreach (var argument in operation.Arguments) {
                    if (argument.Syntax is not ArgumentSyntax syntax || argument.Parameter is null) return [];
                    var name = argument.Parameter.Name;
                    var identifier = SyntaxFacts.GetKeywordKind(name) != SyntaxKind.None || SyntaxFacts.GetContextualKeywordKind(name) != SyntaxKind.None ? "@" + name : name;
                    named[syntax] = syntax.WithNameColon(SyntaxFactory.NameColon(SyntaxFactory.IdentifierName(identifier))).WithAdditionalAnnotations(Formatter.Annotation);
                }
                var mark = new SyntaxAnnotation("Frost.Signature.Call", uses.Count.ToString(System.Globalization.CultureInfo.InvariantCulture));
                Add(callDocument.Id, invocation, invocation.ReplaceNodes(named.Keys, (old, _) => named[old]).WithAdditionalAnnotations(mark));
                uses.Add((callDocument.Id, mark));
            }
        }
        // Recursive invocations overlap the declaration replacement; conservatively withhold this profile.
        if (replacements[document.Id].Keys.Any(n => n != declaration && declaration.Span.Contains(n.Span))) return [];
        var changed = before;
        foreach (var (id, edits) in replacements) {
            var syntax = await before.GetDocument(id)!.GetSyntaxRootAsync(token);
            if (syntax is null) return [];
            changed = changed.WithDocumentSyntaxRoot(id, syntax.ReplaceNodes(edits.Keys, (old, _) => edits[old]));
        }
        foreach (var id in replacements.Keys) changed = (await Formatter.FormatAsync(changed.GetDocument(id)!, cancellationToken: token)).Project.Solution;
        var targetDoc = changed.GetDocument(document.Id)!;
        var targetRoot = await targetDoc.GetSyntaxRootAsync(token);
        var targetModel = await targetDoc.GetSemanticModelAsync(token);
        var targetNode = targetRoot!.GetAnnotatedNodes(target).SingleOrDefault();
        if (targetNode is null || targetModel!.GetDeclaredSymbol(targetNode, token) is not IMethodSymbol expected) return [];
        foreach (var (id, mark) in uses) {
            var callDoc = changed.GetDocument(id)!;
            var syntax = await callDoc.GetSyntaxRootAsync(token); var semantics = await callDoc.GetSemanticModelAsync(token);
            var call = syntax!.GetAnnotatedNodes(mark).SingleOrDefault();
            if (call is null || semantics!.GetSymbolInfo(call, token).Symbol is not IMethodSymbol actual ||
                !SymbolEqualityComparer.Default.Equals(actual.OriginalDefinition, expected.OriginalDefinition)) return [];
        }
        try {
            return [new { title = "Swap first two parameters, preserve evaluation order (Roslyn)", kind = "refactor.rewrite", edit = await context.CreateWorkspaceEditAsync(changed, token) }];
        } catch (InvalidOperationException) { return []; }
          catch (NotSupportedException) { return []; }
    }
}
