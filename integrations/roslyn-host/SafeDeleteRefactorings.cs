// SPDX-License-Identifier: Apache-2.0
// Source-reference-safe private declaration deletion, pinned to the actual symbol and captured solution.
using System.Text.Json;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.FindSymbols;
namespace Frost.RoslynHost;
internal sealed class SafeDeleteRefactorings : ISemanticFeature, ISemanticContributions
{
    public bool Supports(string method) => false;
    public Task<object?> Execute(string method, SemanticFeatureContext context, JsonElement parameters, CancellationToken token) => throw new NotSupportedException(method);
    public async Task<IReadOnlyList<object>> CodeActions(SemanticFeatureContext context, JsonElement parameters, CancellationToken token)
    {
        var document = context.Document;
        var range = await ProviderRefactorings.Selection(document, parameters, token);
        var root = await document.GetSyntaxRootAsync(token); var model = await document.GetSemanticModelAsync(token);
        if (root is null || model is null || root.FullSpan.IsEmpty) return [];
        var selected = root.FindToken(Math.Min(range.Start, root.FullSpan.End - 1));
        var declaration = selected.Parent?.AncestorsAndSelf().FirstOrDefault(n => n is MethodDeclarationSyntax or VariableDeclaratorSyntax or TypeDeclarationSyntax);
        if (declaration is null || declaration.ContainsDirectives || model.GetDeclaredSymbol(declaration, token) is not ISymbol symbol ||
            symbol.IsImplicitlyDeclared || symbol.DeclaredAccessibility != Accessibility.Private || symbol.DeclaringSyntaxReferences.Length != 1 ||
            symbol.GetAttributes().Length != 0 || symbol.Locations.Any(l => !l.IsInSource)) return [];
        SyntaxNode removal;
        switch (symbol) {
        case IMethodSymbol method when declaration is MethodDeclarationSyntax syntax && syntax.Identifier.Span.Contains(range.Start) &&
            method.MethodKind == MethodKind.Ordinary && method.IsStatic && !method.IsVirtual && !method.IsOverride && !method.IsAbstract &&
            !method.IsExtern && method.PartialDefinitionPart is null && method.PartialImplementationPart is null:
            removal = declaration; break;
        case IFieldSymbol field when declaration is VariableDeclaratorSyntax variable && variable.Identifier.Span.Contains(range.Start) &&
            variable.Initializer is null && field.IsStatic && !field.IsConst && variable.Parent?.Parent is FieldDeclarationSyntax fieldDeclaration &&
            fieldDeclaration.Declaration.Variables.Count == 1:
            removal = fieldDeclaration; break;
        case INamedTypeSymbol type when declaration is TypeDeclarationSyntax typeSyntax && typeSyntax.Identifier.Span.Contains(range.Start) &&
            type.ContainingType is not null && !typeSyntax.DescendantNodesAndSelf().OfType<AttributeSyntax>().Any():
            removal = declaration; break;
        default: return [];
        }
        foreach (var references in await SymbolFinder.FindReferencesAsync(symbol, document.Project.Solution, cancellationToken: token))
            if (references.Locations.Any()) return [];
        // Removal is a Roslyn syntax-tree operation against this exact declaration, never a name-based edit.
        var changedRoot = root.RemoveNode(removal, SyntaxRemoveOptions.KeepExteriorTrivia);
        if (changedRoot is null) return [];
        var changed = document.WithSyntaxRoot(changedRoot).Project.Solution;
        try {
            var edit = await context.CreateWorkspaceEditAsync(changed, token);
            return [new { title = "Delete private declaration with no source references (Roslyn)", kind = "refactor.rewrite", edit,
                data = new { project = document.Project.FilePath, assembly = symbol.ContainingAssembly?.Identity.ToString(),
                    declarationId = symbol.GetDocumentationCommentId(), document = document.FilePath, start = declaration.SpanStart, length = declaration.Span.Length } }];
        } catch (InvalidOperationException) { return []; }
          catch (NotSupportedException) { return []; }
    }
}
