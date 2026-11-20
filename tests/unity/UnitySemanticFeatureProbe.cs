// SPDX-License-Identifier: Apache-2.0
// Portable source regression probe. Deferred execution: no Unity installation or project is used.
using System.Text.Json;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Text;
using Frost.RoslynHost;
internal static class UnitySemanticFeatureProbe
{
    private static void Require(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
    public static async Task Main() {
        using var workspace = new AdhocWorkspace();
        var project = workspace.AddProject(ProjectInfo.Create(ProjectId.CreateNewId(), VersionStamp.Create(), "UnityFixture", "UnityFixture", LanguageNames.CSharp,
            filePath: Path.Combine(Path.GetTempPath(), "UnityFixture.csproj"), compilationOptions: new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary),
            metadataReferences: new[] { MetadataReference.CreateFromFile(typeof(object).Assembly.Location) }));
        workspace.AddDocument(project.Id, "UnityTypes.cs", SourceText.From("namespace UnityEngine { public class Object { public static Object FindObjectOfType() => new Object(); } public class MonoBehaviour : Object {} public class ScriptableObject : Object {} public sealed class SerializeField : System.Attribute {} }"));
        var source = """
            class Component : UnityEngine.MonoBehaviour {
                [UnityEngine.SerializeField] private int health;
                private void Update() { UnityEngine.Object.FindObjectOfType(); }
                private static void Awake() {}
                public void Click() {}
            }
            class Unrelated { private void Update() {} }
            """;
        var document = workspace.AddDocument(project.Id, "Component.cs", SourceText.From(source));
        var feature = new UnitySemanticFeature();
        var parameters = JsonSerializer.SerializeToElement(new { position = new { line = 0, character = 0 } });
        Solution? proposed = null;
        var context = new SemanticFeatureContext(document, (changed, _) => { proposed = changed; return Task.FromResult<object>(new { admitted = true }); });
        var facts = JsonSerializer.SerializeToElement(await feature.Execute("frost/unity/semantics", context, parameters, CancellationToken.None));
        var types = facts.GetProperty("types").EnumerateArray().ToArray();
        Require(types.Length == 1, "Only genuine Unity-derived types may receive Unity semantics");
        Require(types[0].GetProperty("fields").GetArrayLength() == 1, "Serialized private field has authoritative symbol evidence");
        Require(types[0].GetProperty("implicitUses").GetArrayLength() == 1, "Static Awake is not implicitly used");
        Require(types[0].GetProperty("inspections").GetArrayLength() == 1, "Incorrect callback signature is diagnosed");
        Require(types[0].GetProperty("hotspots").GetArrayLength() == 1, "Actual Unity object lookup in Update is highlighted");
        Require(types[0].GetProperty("fields")[0].GetProperty("identity").GetProperty("declarationId").GetString() == "F:Component.health", "Serialized field uses Roslyn declaration identity");
        var className = JsonSerializer.SerializeToElement(new { position = new { line = 0, character = 7 } });
        var selectedType = JsonSerializer.SerializeToElement(await feature.Execute("frost/unity/semantics", context, className, CancellationToken.None));
        Require(selectedType.GetProperty("selectedMember").GetProperty("kind").GetString() == "asset", "Declared class selection retains original script asset usages");
        Require(selectedType.GetProperty("selectedMember").GetProperty("identity").GetProperty("declarationId").GetString() == "T:Component", "Selected script type has declared Roslyn identity");
        var headerSource = "using System;\nclass First : UnityEngine.MonoBehaviour {}\nclass Second : UnityEngine.MonoBehaviour {}\n";
        var headerDocument = document.WithText(SourceText.From(headerSource));
        var headerContext = new SemanticFeatureContext(headerDocument, (_, _) => throw new InvalidOperationException("Header generation must never reach proposal adoption"));
        var rejected = false;
        try {
            await feature.Execute("frost/unity/generateEvent", headerContext,
                JsonSerializer.SerializeToElement(new { position = new { line = 0, character = 2 }, eventName = "OnEnable" }), CancellationToken.None);
        } catch (ArgumentException) { rejected = true; }
        Require(rejected, "Generation outside one explicitly selected enclosing type is refused");
        var headerActions = await feature.CodeActions(headerContext, JsonSerializer.SerializeToElement(new {
            range = new { start = new { line = 0, character = 2 }, end = new { line = 0, character = 2 } }
        }), CancellationToken.None);
        Require(headerActions.Count == 0, "Normal intentions cannot silently choose the first of multiple Unity types");
        var generation = JsonSerializer.SerializeToElement(new { position = new { line = 2, character = 4 }, eventName = "OnEnable" });
        await feature.Execute("frost/unity/generateEvent", context, generation, CancellationToken.None);
        Require(proposed is not null, "Generation must pass through the admitted proposal callback");
        var changedText = await proposed!.GetDocument(document.Id)!.GetTextAsync();
        Require(changedText.ToString().Contains("void OnEnable()", StringComparison.Ordinal), "Proposed event function is generated");
        Require((await document.GetTextAsync()).ToString() == source, "Original captured document remains unchanged");
        Require((await feature.CodeLenses(context, CancellationToken.None)).Count == 2, "Normal Code Vision contributions expose event and field evidence");
        Require((await feature.Diagnostics(context, CancellationToken.None)).Count == 2, "Normal diagnostics include signature and hotspot evidence");
    }
}
