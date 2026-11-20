// SPDX-License-Identifier: Apache-2.0
// Acceptance probe emits real portable PDB/embedded source and reads actual ILSpy output; never executes target code.
using System.Buffers.Binary;
using System.Reflection.Metadata;
using System.Reflection.PortableExecutable;
using System.Text;
using System.Text.Json;
using Frost.RoslynHost;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Emit;
using Microsoft.CodeAnalysis.Text;
internal static class Program
{
    private static void Require(bool value, string message) { if (!value) throw new Exception(message); }
    private static JsonElement Json(object value) => JsonSerializer.SerializeToElement(value);
    private static async Task Main(string[] args)
    {
        if (args.Length != 1 || !Path.IsPathFullyQualified(args[0]) || !Directory.Exists(args[0])) throw new ArgumentException("owned existing scratch directory required");
        string root = args[0], assembly = Path.Combine(root, "Library.dll"), pdb = Path.Combine(root, "Library.pdb");
        const string original = "public static class Library { public static int Value() => 73; }\n";
        var source = SourceText.From(original, new UTF8Encoding(false), SourceHashAlgorithm.Sha256);
        var tree = CSharpSyntaxTree.ParseText(source, path: Path.Combine(root, "Library.cs"));
        var compilation = CSharpCompilation.Create("Library", new[] { tree },
            new[] { MetadataReference.CreateFromFile(typeof(object).Assembly.Location) },
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, optimizationLevel: OptimizationLevel.Debug));
        using (var pe = File.Create(assembly)) using (var symbols = File.Create(pdb)) {
            var emitted = compilation.Emit(pe, symbols, options: new EmitOptions(debugInformationFormat: DebugInformationFormat.PortablePdb),
                embeddedTexts: new[] { EmbeddedText.FromSource(tree.FilePath, source) });
            Require(emitted.Success, string.Join("\n", emitted.Diagnostics));
        }
        var consumer = CSharpCompilation.Create("Consumer", references: new[] { MetadataReference.CreateFromFile(assembly), MetadataReference.CreateFromFile(typeof(object).Assembly.Location) });
        var type = consumer.GetTypeByMetadataName("Library") ?? throw new Exception("fixture type missing");
        var method = type.GetMembers("Value").Single();
        var embedded = Json(await LibrarySources.Read(method, assembly, "auto", false, default));
        Require(embedded.GetProperty("kind").GetString() == "embedded", "real embedded source was not selected");
        Require(embedded.GetProperty("source").GetString() == original, "embedded source differs from compiler input bytes");
        var decompiled = Json(await LibrarySources.Read(method, assembly, "decompiled", false, default));
        Require(decompiled.GetProperty("kind").GetString() == "decompiled", "decompiler result mislabeled");
        Require(decompiled.GetProperty("source").GetString()!.Contains("73"), "actual decompilation lost fixture return constant");
        Require(decompiled.GetProperty("identity").GetString() != embedded.GetProperty("identity").GetString(), "different source providers collide in cache");
        var il = Json(await LibrarySources.Read(method, assembly, "il", false, default));
        using (var input = File.OpenRead(assembly)) using (var pe = new PEReader(input)) {
            var metadata = pe.GetMetadataReader();
            var body = metadata.GetTypeDefinition(metadata.TypeDefinitions.Single(h => metadata.GetString(metadata.GetTypeDefinition(h).Name) == "Library")).GetMethods().Single();
            var bytes = pe.GetMethodBody(metadata.GetMethodDefinition(body).RelativeVirtualAddress).GetILBytes()!;
            Require(il.GetProperty("source").GetString()!.Contains(Convert.ToHexString(bytes.AsSpan(0, Math.Min(16, bytes.Length)))), "IL view differs from actual PE method bytes");
        }
        var explorer = Json(await LibrarySources.Read(method, assembly, "assembly", false, default));
        Require(explorer.GetProperty("source").GetString()!.Contains("Library"), "metadata explorer omits actual type");
        byte[] map = Encoding.UTF8.GetBytes("{\"documents\":{\"/src/*\":\"https://raw.githubusercontent.com/owner/repo/commit/*\"}}");
        Require(LibrarySources.SourceLinkUrl(map, "/src/two words/a.cs") == "https://raw.githubusercontent.com/owner/repo/commit/two%20words/a.cs", "SourceLink mapping/URL encoding failed");
        Require(LibrarySources.SourceLinkUrl(map, "/other/a.cs") is null, "unmapped document acquired a URL");
        bool refused = false;
        try { LibrarySources.SourceLinkUrl(map, "/src/../escape.cs"); } catch (InvalidDataException) { refused = true; }
        Require(refused, "SourceLink traversal accepted");
        refused = false;
        try { LibrarySources.SourceLinkUrl(Encoding.UTF8.GetBytes("{\"documents\":{\"/src/*\":\"https://127.0.0.1/*\"}}"), "/src/a.cs"); } catch (NotSupportedException) { refused = true; }
        Require(refused, "private SourceLink origin accepted");
        byte[] bomb = new byte[4]; BinaryPrimitives.WriteInt32LittleEndian(bomb, LibrarySources.MaxSource + 1);
        refused = false; try { LibrarySources.DecodeEmbedded(bomb); } catch (InvalidDataException) { refused = true; }
        Require(refused, "embedded source expansion budget ignored");
        using var cancelled = new CancellationTokenSource(); cancelled.Cancel();
        refused = false; try { await LibrarySources.Read(method, assembly, "decompiled", false, cancelled.Token); } catch (OperationCanceledException) { refused = true; }
        Require(refused, "cancelled decompilation continued");
        Console.WriteLine("LibrarySourceProbe passed: real PE, portable PDB, embedded source, decompilation, IL, metadata and source policy");
    }
}
