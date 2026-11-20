// SPDX-License-Identifier: Apache-2.0
// Owned semantic extensions share evaluated Roslyn documents and the validated text proposal boundary.
using System.Text.Json;
using Microsoft.CodeAnalysis;
namespace Frost.RoslynHost;
internal interface ISemanticFeature
{
    bool Supports(string method);
    Task<object?> Execute(string method, SemanticFeatureContext context, JsonElement parameters, CancellationToken token);
}
internal sealed class SemanticFeatureContext
{
    internal Document Document { get; }
    private readonly Func<Solution, CancellationToken, Task<object>> proposal;
    internal SemanticFeatureContext(Document document, Func<Solution, CancellationToken, Task<object>> createProposal)
    { Document = document; proposal = createProposal; }
    // Returns a versioned WorkspaceEdit proposal only, after source/compiler/conflict admission.
    // The caller must not mutate files or invoke Workspace.TryApplyChanges; REF1 owns adoption.
    internal Task<object> CreateWorkspaceEditAsync(Solution changed, CancellationToken token) => proposal(changed, token);
}
