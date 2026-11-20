// SPDX-License-Identifier: Apache-2.0
// Bounded semantic local transformations. Every proposal crosses the shared compiler/REF1 gate.
using System.Text.Json;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.FindSymbols;
using Microsoft.CodeAnalysis.Formatting;
using Microsoft.CodeAnalysis.Text;
namespace Frost.RoslynHost;
internal sealed class LocalRefactorings : ISemanticFeature, ISemanticContributions
{
    public bool Supports(string method) => false;
    public Task<object?> Execute(string method, SemanticFeatureContext context, JsonElement parameters, CancellationToken token) => throw new NotSupportedException(method);
    public async Task<IReadOnlyList<object>> CodeActions(SemanticFeatureContext context, JsonElement parameters, CancellationToken token)
    {
        var document = context.Document;
        var root = await document.GetSyntaxRootAsync(token);
        var semantic = await document.GetSemanticModelAsync(token);
        var text = await document.GetTextAsync(token);
        if (root is null || semantic is null || !parameters.TryGetProperty("range", out var range)) return Array.Empty<object>();
        int Offset(JsonElement p) {
            var line = p.GetProperty("line").GetInt32(); var column = p.GetProperty("character").GetInt32();
            if (line < 0 || line >= text.Lines.Count || column < 0 || column > text.Lines[line].Span.Length) throw new InvalidDataException("Refactoring range outside document");
            return text.Lines[line].Start + column;
        }
        var start = Offset(range.GetProperty("start")); var end = Offset(range.GetProperty("end"));
        if (end < start) throw new InvalidDataException("Reversed refactoring range");
        var selection = TextSpan.FromBounds(start, end);
        var actions = new List<object>();
        async Task Propose(string title, string kind, SyntaxNode changed) {
            var formatted = await Formatter.FormatAsync(document.WithSyntaxRoot(changed), cancellationToken: token);
            try { actions.Add(new { title, kind, edit = await context.CreateWorkspaceEditAsync(formatted.Project.Solution, token) }); }
            catch (InvalidOperationException) { /* A semantic conflict means this bounded transformation is unavailable. */ }
            catch (NotSupportedException) { /* Unsupported project/file changes cannot enter REF1. */ }
        }
        // Exact whole-return selection preserves evaluation order and never hoists across a statement.
        var expression = root.FindNode(selection, getInnermostNodeForTie: true) as ExpressionSyntax;
        if (!semantic.LookupNamespacesAndTypes(start, name: "var").Any() && !selection.IsEmpty && expression is not null && expression.Span == selection &&
            expression.Parent is ReturnStatementSyntax returned && returned.Parent is BlockSyntax block &&
            expression is not RefExpressionSyntax && !returned.ContainsDirectives &&
            semantic.GetTypeInfo(expression, token).Type is { TypeKind: not TypeKind.Error } type && type.SpecialType != SpecialType.System_Void) {
            var name = "value";
            for (var suffix = 1; semantic.LookupSymbols(start, name: name).Length != 0 ||
                root.DescendantTokens().Any(t => t.IsKind(SyntaxKind.IdentifierToken) && t.ValueText == name); ++suffix) name = "value" + suffix;
            var local = SyntaxFactory.LocalDeclarationStatement(SyntaxFactory.VariableDeclaration(SyntaxFactory.IdentifierName("var"),
                SyntaxFactory.SingletonSeparatedList(SyntaxFactory.VariableDeclarator(name).WithInitializer(SyntaxFactory.EqualsValueClause(expression)))))
                .WithLeadingTrivia(returned.GetLeadingTrivia()).WithTrailingTrivia(SyntaxFactory.ElasticCarriageReturnLineFeed).WithAdditionalAnnotations(Formatter.Annotation);
            var replacement = returned.WithExpression(SyntaxFactory.IdentifierName(name)).WithoutLeadingTrivia().WithAdditionalAnnotations(Formatter.Annotation);
            var index = block.Statements.IndexOf(returned);
            var changedBlock = block.WithStatements(block.Statements.Replace(returned, replacement).Insert(index, local));
            await Propose("Introduce local for return value (Roslyn)", "refactor.extract", root.ReplaceNode(block, changedBlock));
        }
        // Only var + immediately following return of its sole semantic reference is eligible.
        var identifier = root.FindToken(Math.Min(start, Math.Max(0, root.FullSpan.End - 1))).Parent as IdentifierNameSyntax;
        if (identifier?.Parent is ReturnStatementSyntax final && final.Expression == identifier && final.Parent is BlockSyntax enclosing) {
            var index = enclosing.Statements.IndexOf(final);
            if (index > 0 && enclosing.Statements[index - 1] is LocalDeclarationStatementSyntax declaration &&
                declaration.Declaration.Type.IsVar && !semantic.LookupNamespacesAndTypes(declaration.SpanStart, name: "var").Any() && declaration.Declaration.Variables.Count == 1 && declaration.Modifiers.Count == 0 &&
                declaration.UsingKeyword.IsKind(SyntaxKind.None) && declaration.AwaitKeyword.IsKind(SyntaxKind.None) &&
                !declaration.ContainsDirectives && !final.ContainsDirectives &&
                !declaration.DescendantTrivia().Concat(final.DescendantTrivia()).Any(t => !t.IsKind(SyntaxKind.WhitespaceTrivia) && !t.IsKind(SyntaxKind.EndOfLineTrivia)) &&
                declaration.Declaration.Variables[0] is var variable && variable.Initializer is not null &&
                semantic.GetDeclaredSymbol(variable, token) is ILocalSymbol symbol &&
                SymbolEqualityComparer.Default.Equals(semantic.GetSymbolInfo(identifier, token).Symbol, symbol)) {
                var references = await SymbolFinder.FindReferencesAsync(symbol, document.Project.Solution, cancellationToken: token);
                var locations = references.SelectMany(reference => reference.Locations).ToArray();
                if (locations.Length == 1 && locations[0].Document.Id == document.Id && locations[0].Location.SourceSpan == identifier.Span) {
                    var replacement = final.WithExpression(variable.Initializer.Value.WithTriviaFrom(identifier))
                        .WithLeadingTrivia(declaration.GetLeadingTrivia().AddRange(final.GetLeadingTrivia())).WithAdditionalAnnotations(Formatter.Annotation);
                    await Propose("Inline return local (Roslyn)", "refactor.inline", root.ReplaceNode(enclosing,
                        enclosing.WithStatements(enclosing.Statements.RemoveAt(index - 1).Replace(final, replacement))));
                }
            }
        }
        return actions;
    }
}
