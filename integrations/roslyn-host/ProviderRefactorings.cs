// SPDX-License-Identifier: Apache-2.0
// Actual installed Roslyn extract-method providers; operations are proposals and never applied here.
using System.Text.Json;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CodeActions;
using Microsoft.CodeAnalysis.CodeRefactorings;
using Microsoft.CodeAnalysis.Text;
namespace Frost.RoslynHost;
internal sealed class ProviderRefactorings : ISemanticFeature, ISemanticContributions
{
    public bool Supports(string method) => false;
    public Task<object?> Execute(string method, SemanticFeatureContext context, JsonElement parameters, CancellationToken token) => throw new NotSupportedException(method);
    internal static async Task<TextSpan> Selection(Document document, JsonElement parameters, CancellationToken token)
    {
        var text = await document.GetTextAsync(token);
        var range = parameters.GetProperty("range");
        int Offset(JsonElement position) {
            var line = position.GetProperty("line").GetInt32(); var column = position.GetProperty("character").GetInt32();
            if (line < 0 || line >= text.Lines.Count || column < 0 || column > text.Lines[line].Span.Length)
                throw new InvalidDataException("Refactoring selection outside UTF-16 document");
            return text.Lines[line].Start + column;
        }
        var start = Offset(range.GetProperty("start")); var end = Offset(range.GetProperty("end"));
        if (end < start) throw new InvalidDataException("Reversed refactoring selection");
        return TextSpan.FromBounds(start, end);
    }
    public async Task<IReadOnlyList<object>> CodeActions(SemanticFeatureContext context, JsonElement parameters, CancellationToken token)
    {
        var selection = await Selection(context.Document, parameters, token);
        var proposed = new List<(CodeAction Action, string Kind)>();
        // Roslyn owns selection/data-flow eligibility, capture/ref/out/async and target placement.
        foreach (var (name, kind) in new[] { ("Extract Method Code Action Provider", "refactor.extract"), ("Move Type To File Code Action Provider", "refactor.move") }) {
            if (selection.IsEmpty && kind == "refactor.extract") continue;
            foreach (var provider in context.Providers(name))
                await provider.ComputeRefactoringsAsync(new CodeRefactoringContext(context.Document, selection, action => proposed.Add((action, kind)), token));
        }
        var result = new List<object>();
        async Task Visit(CodeAction action, string kind, int depth) {
            token.ThrowIfCancellationRequested();
            if (depth > 4 || result.Count >= 32) throw new InvalidOperationException("Roslyn refactoring action budget exceeded");
            if (!action.NestedActions.IsDefaultOrEmpty) {
                foreach (var child in action.NestedActions) await Visit(child, kind, depth + 1);
                return;
            }
            var operations = await action.GetOperationsAsync(token);
            // Never run provider operations (dialogs, commands, workspace mutation or file writes).
            var applies = operations.OfType<ApplyChangesOperation>().ToArray();
            if (applies.Length != 1 || operations.Any(operation => operation is not ApplyChangesOperation &&
                operation.GetType().FullName != "Microsoft.CodeAnalysis.CodeActions.OpenDocumentOperation")) return;
            var apply = applies[0];
            try {
                var edit = await context.CreateWorkspaceEditAsync(apply.ChangedSolution, token);
                result.Add(new { title = action.Title + " (Roslyn)", kind, edit });
            } catch (NotSupportedException) { /* REF1 cannot adopt unsupported solution/resource deltas. */ }
              catch (InvalidOperationException) { /* Compiler/source/conflict validation rejected this proposal. */ }
        }
        foreach (var (action, kind) in proposed) await Visit(action, kind, 0);
        return result;
    }
}
