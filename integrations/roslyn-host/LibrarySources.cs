// SPDX-License-Identifier: Apache-2.0
// Read actual PE/PDB bytes and the already audited ILSpy engine; never load target assemblies as code.
using System.Buffers.Binary;
using System.IO.Compression;
using System.Net;
using System.Reflection.Metadata;
using System.Reflection.Metadata.Ecma335;
using System.Reflection.PortableExecutable;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using ICSharpCode.Decompiler;
using ICSharpCode.Decompiler.CSharp;
using ICSharpCode.Decompiler.Metadata;
using Microsoft.CodeAnalysis;
namespace Frost.RoslynHost;
internal static class LibrarySources
{
    internal const int MaxSource = 4 * 1024 * 1024;
    private const int MaxAssembly = 256 * 1024 * 1024;
    private static readonly Guid EmbeddedSource = new("0e8a571b-6926-466e-b4ad-8ab04611f5fe");
    private static readonly Guid SourceLink = new("cc110556-a091-4d38-9fec-25ab9a351a6a");
    private static readonly Guid Sha256 = new("8829d00f-11b8-4213-878b-770e8597ac16");
    private static readonly UTF8Encoding Utf8 = new(false, true);
    private static string Hash(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
    private static byte[] ReadBounded(string path, int limit)
    {
        using var input = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        if (input.Length < 1 || input.Length > limit) throw new InvalidDataException("Library input exceeds its size budget");
        var bytes = new byte[checked((int)input.Length)]; input.ReadExactly(bytes);
        if (input.ReadByte() != -1) throw new InvalidDataException("Library input changed while reading");
        return bytes;
    }
    private static string TypeName(MetadataReader reader, TypeDefinitionHandle handle, int depth = 0)
    {
        if (depth > 64) throw new InvalidDataException("Library type nesting exceeds budget");
        var type = reader.GetTypeDefinition(handle); var parent = type.GetDeclaringType();
        string prefix = parent.IsNil ? reader.GetString(type.Namespace) : TypeName(reader, parent, depth + 1);
        return prefix.Length == 0 ? reader.GetString(type.Name) : prefix + (parent.IsNil ? "." : "+") + reader.GetString(type.Name);
    }
    private static string TypeName(INamedTypeSymbol type) => type.ContainingType is not null
        ? TypeName(type.ContainingType) + "+" + type.MetadataName
        : (type.ContainingNamespace.IsGlobalNamespace ? "" : type.ContainingNamespace.ToDisplayString() + ".") + type.MetadataName;

    internal static async Task<object> Read(ISymbol symbol, string assembly, string mode, bool allowSourceLink, CancellationToken token, Guid? expectedModule = null)
    {
        if (mode is not ("auto" or "sourceLink" or "decompiled" or "il" or "assembly")) throw new NotSupportedException("Unknown library source mode");
        token.ThrowIfCancellationRequested();
        byte[] image = ReadBounded(assembly, MaxAssembly); string assemblyHash = Hash(image);
        using var pe = new PEReader(new MemoryStream(image, writable: false));
        var metadata = pe.GetMetadataReader();
        var assemblyDefinition = metadata.GetAssemblyDefinition();
        if (metadata.GetString(assemblyDefinition.Name) != symbol.ContainingAssembly.Identity.Name ||
            assemblyDefinition.Version != symbol.ContainingAssembly.Identity.Version)
            throw new InvalidDataException("Evaluated symbol assembly identity differs from current image");
        var containingType = symbol as INamedTypeSymbol ?? symbol.ContainingType ?? throw new NotSupportedException("Library symbol has no declaring type");
        var types = metadata.TypeDefinitions.Where(h => TypeName(metadata, h) == TypeName(containingType)).ToArray();
        if (types.Length != 1) throw new InvalidDataException("Library symbol type does not match the evaluated PE image");
        var typeHandle = types[0]; var type = metadata.GetTypeDefinition(typeHandle);
        var mvid = metadata.GetGuid(metadata.GetModuleDefinition().Mvid);
        if (expectedModule is not null && mvid != expectedModule.Value)
            throw new InvalidDataException("Library module changed since compiler reference evaluation");
        string module = mvid.ToString();
        string documentationId = DocumentationCommentId.CreateDeclarationId(symbol) ?? symbol.ToDisplayString();
        string kind = mode == "assembly" ? "assembly" : mode == "il" ? "il" : "decompiled";
        string provenance = "assembly SHA256 " + assemblyHash + "; module " + module;
        string source; int line = 0;
        if (mode == "assembly") {
            var listing = new StringBuilder("// Actual PE metadata; not reconstructed C#\n// " + provenance + "\n");
            foreach (var h in metadata.TypeDefinitions) { token.ThrowIfCancellationRequested(); listing.AppendLine($"0x{MetadataTokens.GetToken(h):x8} {TypeName(metadata, h)}"); if (listing.Length > MaxSource) throw new InvalidDataException("Assembly Explorer exceeds output budget"); }
            source = listing.ToString();
        } else if (mode == "il") {
            var listing = new StringBuilder("// Raw IL bytes by MethodDef token; operands are not reconstructed C#\n// " + provenance + "\n");
            foreach (var h in type.GetMethods()) {
                token.ThrowIfCancellationRequested(); var method = metadata.GetMethodDefinition(h);
                listing.AppendLine($".method 0x{MetadataTokens.GetToken(h):x8} {metadata.GetString(method.Name)}");
                if (method.RelativeVirtualAddress == 0) { listing.AppendLine("// No IL body in this image"); continue; }
                var il = pe.GetMethodBody(method.RelativeVirtualAddress).GetILBytes() ?? Array.Empty<byte>();
                for (int offset = 0; offset < il.Length; offset += 16) {
                    listing.AppendLine($"IL_{offset:x4}: {Convert.ToHexString(il.AsSpan(offset, Math.Min(16, il.Length - offset)))}");
                    if (listing.Length > MaxSource) throw new InvalidDataException("IL view exceeds output budget");
                }
            }
            source = listing.ToString();
        } else {
            var original = mode is "auto" or "sourceLink" ? await OriginalSource(pe, metadata, typeHandle, symbol, assembly, allowSourceLink, token) : null;
            if (original is not null) { source = original.Value.Source; line = original.Value.Line; kind = original.Value.Kind; provenance += "; " + original.Value.Provenance; }
            else {
                if (mode == "sourceLink") throw new NotSupportedException("No verified embedded/SourceLink source is available for this exact member");
                // The audited package is already included by the verified managed-server payload.
                // API contract: ILSpy v9.1 CSharpDecompiler.DecompileAsString(EntityHandle[]).
                using var file = new PEFile(assembly, new MemoryStream(image, writable: false), PEStreamOptions.PrefetchEntireImage);
                var resolver = new UniversalAssemblyResolver(assembly, false, null);
                var decompiler = new CSharpDecompiler(file, resolver, new DecompilerSettings()) { CancellationToken = token };
                source = "// Decompiled by ICSharpCode.Decompiler 9.1.0.7988; reconstructed, not original source\n// " + provenance + "\n" + decompiler.DecompileAsString(typeHandle);
            }
        }
        token.ThrowIfCancellationRequested();
        byte[] output = Utf8.GetBytes(source);
        if (output.Length == 0 || output.Length > MaxSource) throw new InvalidDataException("Library source exceeds output budget");
        // Bind publication to the exact current assembly, even when it was replaced during work.
        if (Hash(ReadBounded(assembly, MaxAssembly)) != assemblyHash) throw new InvalidDataException("Library assembly changed during navigation");
        string sourceHash = Hash(output);
        string identity = Hash(Utf8.GetBytes(assemblyHash + "\n" + documentationId + "\n" + kind + "\n" + sourceHash));
        return new { source, identity, sourceSha256 = sourceHash, kind, provenance, documentationId, assemblySha256 = assemblyHash, moduleId = module,
            extension = kind is "il" or "assembly" ? ".il" : ".cs", line, character = 0 };
    }

    private static async Task<(string Source, int Line, string Kind, string Provenance)?> OriginalSource(
        PEReader pe, MetadataReader metadata, TypeDefinitionHandle typeHandle, ISymbol symbol, string assembly, bool allowSourceLink, CancellationToken token, Guid? expectedModule = null)
    {
        string pdbPath = Path.ChangeExtension(assembly, ".pdb");
        MetadataReaderProvider? provider = null; byte[]? external = null;
        var entries = pe.ReadDebugDirectory();
        try {
            var embedded = entries.Where(e => e.Type == DebugDirectoryEntryType.EmbeddedPortablePdb).ToArray();
            if (embedded.Length == 1) {
                var prefix = pe.GetSectionData(embedded[0].DataRelativeVirtualAddress).GetContent(0, Math.Min(8, embedded[0].DataSize));
                if (prefix.Length != 8 || BinaryPrimitives.ReadUInt32LittleEndian(prefix.AsSpan()) != 0x4244504d ||
                    BinaryPrimitives.ReadUInt32LittleEndian(prefix.AsSpan().Slice(4)) > MaxAssembly)
                    throw new InvalidDataException("Embedded PDB expansion exceeds budget");
                provider = pe.ReadEmbeddedPortablePdbDebugDirectoryData(embedded[0]);
            }
            else if (embedded.Length > 1) throw new InvalidDataException("Ambiguous embedded portable PDB");
            else if (File.Exists(pdbPath)) { external = ReadBounded(pdbPath, MaxAssembly); provider = MetadataReaderProvider.FromPortablePdbStream(new MemoryStream(external, writable: false)); }
            if (provider is null) return null;
            var pdb = provider.GetMetadataReader(); var header = pdb.DebugMetadataHeader ?? throw new InvalidDataException("PDB has no identity");
            var id = new BlobContentId(header.Id);
            if (!entries.Any(e => e.Type == DebugDirectoryEntryType.CodeView && pe.ReadCodeViewDebugDirectoryData(e).Guid == id.Guid && e.Stamp == id.Stamp && pe.ReadCodeViewDebugDirectoryData(e).Age == 1))
                throw new InvalidDataException("Library PDB does not match its PE image");
            if (external is not null) {
                var checksumBytes = (byte[])external.Clone(); Array.Clear(checksumBytes, header.IdStartOffset, 20);
                bool bound = entries.Any(e => e.Type == DebugDirectoryEntryType.PdbChecksum && MatchPdbChecksum(pe.ReadPdbChecksumDebugDirectoryData(e), checksumBytes));
                if (!bound) throw new InvalidDataException("Library PDB checksum does not match its PE image");
            }
            var methods = metadata.GetTypeDefinition(typeHandle).GetMethods().ToArray();
            if (symbol is IMethodSymbol methodSymbol) methods = methods.Where(h => {
                var method = metadata.GetMethodDefinition(h);
                return metadata.GetString(method.Name) == methodSymbol.MetadataName && method.GetParameters().Count(p => metadata.GetParameter(p).SequenceNumber > 0) == methodSymbol.Parameters.Length;
            }).ToArray();
            // Do not navigate to a guessed overload or choose one document for a partial type.
            if (methods.Length != 1) return null;
            var info = pdb.GetMethodDebugInformation(MetadataTokens.MethodDebugInformationHandle(MetadataTokens.GetRowNumber(methods[0])));
            var points = info.GetSequencePoints().Where(p => !p.IsHidden).ToArray();
            if (points.Length == 0) return null;
            var documentHandle = points[0].Document.IsNil ? info.Document : points[0].Document;
            if (documentHandle.IsNil) return null;
            var document = pdb.GetDocument(documentHandle);
            if (pdb.GetGuid(document.HashAlgorithm) != Sha256) return null;
            byte[] expected = pdb.GetBlobBytes(document.Hash); byte[]? bytes = null; string kind = "embedded"; string origin = "portable PDB embedded source";
            foreach (var customHandle in pdb.GetCustomDebugInformation(documentHandle)) {
                var custom = pdb.GetCustomDebugInformation(customHandle);
                if (pdb.GetGuid(custom.Kind) != EmbeddedSource) continue;
                if (bytes is not null) throw new InvalidDataException("Duplicate embedded source");
                bytes = DecodeEmbedded(pdb.GetBlobBytes(custom.Value));
            }
            if (bytes is null && allowSourceLink) {
                string name = pdb.GetString(document.Name); string? url = null;
                foreach (var h in pdb.GetCustomDebugInformation(MetadataTokens.EntityHandle(0x00000001))) {
                    var custom = pdb.GetCustomDebugInformation(h);
                    if (pdb.GetGuid(custom.Kind) != SourceLink) continue;
                    if (url is not null) throw new InvalidDataException("Ambiguous SourceLink map");
                    url = SourceLinkUrl(pdb.GetBlobBytes(custom.Value), name);
                }
                if (url is not null) { bytes = await FetchSource(new Uri(url), token); kind = "sourceLink"; origin = url; }
            }
            if (bytes is null) return null;
            if (!CryptographicOperations.FixedTimeEquals(SHA256.HashData(bytes), expected)) throw new InvalidDataException("Library document checksum mismatch");
            return (Utf8.GetString(bytes), Math.Max(0, points[0].StartLine - 1), kind, origin + "; PDB SHA256 " + (external is null ? "embedded" : Hash(external)));
        } finally { provider?.Dispose(); }
    }
    private static bool MatchPdbChecksum(PdbChecksumDebugDirectoryData checksum, byte[] bytes)
    {
        byte[]? hash = checksum.AlgorithmName switch { "SHA256" => SHA256.HashData(bytes), "SHA384" => SHA384.HashData(bytes), "SHA512" => SHA512.HashData(bytes), _ => null };
        return hash is not null && CryptographicOperations.FixedTimeEquals(hash, checksum.Checksum.AsSpan());
    }
    internal static byte[] DecodeEmbedded(byte[] blob)
    {
        if (blob.Length < 4 || blob.Length > MaxSource + 4) throw new InvalidDataException("Invalid embedded source size");
        int length = BinaryPrimitives.ReadInt32LittleEndian(blob);
        if (length == 0) return blob.AsSpan(4).ToArray();
        if (length < 0 || length > MaxSource) throw new InvalidDataException("Embedded source expansion exceeds budget");
        using var compressed = new MemoryStream(blob, 4, blob.Length - 4, writable: false);
        using var inflate = new DeflateStream(compressed, CompressionMode.Decompress);
        var result = new byte[length]; inflate.ReadExactly(result);
        if (inflate.ReadByte() != -1) throw new InvalidDataException("Embedded source length mismatch");
        return result;
    }
    internal static string? SourceLinkUrl(byte[] json, string document)
    {
        if (json.Length > 1024 * 1024) throw new InvalidDataException("SourceLink map exceeds budget");
        using var parsed = JsonDocument.Parse(json); string? result = null; int best = -1;
        foreach (var map in parsed.RootElement.GetProperty("documents").EnumerateObject()) {
            var value = map.Value.GetString() ?? throw new InvalidDataException("Invalid SourceLink URL");
            int star = map.Name.IndexOf('*'); string? suffix = null;
            if (star < 0) { if (map.Name == document) suffix = ""; }
            else if (star == map.Name.Length - 1 && document.StartsWith(map.Name[..star], StringComparison.Ordinal)) suffix = document[star..];
            if (suffix is null) continue;
            int specificity = star < 0 ? int.MaxValue : star;
            if (specificity < best) continue;
            if (specificity == best) throw new InvalidDataException("Ambiguous SourceLink document map");
            if (suffix.Split('/', '\\').Any(part => part is "." or "..")) throw new InvalidDataException("Unsafe SourceLink relative path");
            string encoded = string.Join("/", suffix.Replace('\\', '/').Split('/').Select(Uri.EscapeDataString));
            if (star >= 0 && value.Count(c => c == '*') != 1) throw new InvalidDataException("Invalid SourceLink wildcard");
            result = star >= 0 ? value.Replace("*", encoded, StringComparison.Ordinal) : value; best = specificity;
        }
        if (result is not null) RequireSourceOrigin(new Uri(result));
        return result;
    }
    private static void RequireSourceOrigin(Uri uri)
    {
        // No local/private endpoints, credentials, query secrets, redirects, cookies or proxy auth.
        // Broader/private SourceLink providers require an explicit separate origin policy.
        if (uri.Scheme != "https" || uri.Port != 443 || uri.UserInfo.Length != 0 || uri.Query.Length != 0 || uri.Fragment.Length != 0 ||
            uri.Host != "raw.githubusercontent.com") throw new NotSupportedException("SourceLink origin is not permitted; only public raw.githubusercontent.com HTTPS is enabled");
    }
    private static async Task<byte[]> FetchSource(Uri uri, CancellationToken token)
    {
        RequireSourceOrigin(uri);
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(token);
        deadline.CancelAfter(TimeSpan.FromSeconds(20));
        token = deadline.Token;
        using var handler = new HttpClientHandler { AllowAutoRedirect = false, UseCookies = false, UseDefaultCredentials = false, UseProxy = false, AutomaticDecompression = DecompressionMethods.None };
        using var client = new HttpClient(handler) { Timeout = TimeSpan.FromSeconds(20) };
        using var response = await client.GetAsync(uri, HttpCompletionOption.ResponseHeadersRead, token); response.EnsureSuccessStatusCode();
        if (response.Content.Headers.ContentLength > MaxSource) throw new InvalidDataException("SourceLink body exceeds budget");
        using var input = await response.Content.ReadAsStreamAsync(token);
        return await ReadSourceBody(input, token);
    }
    internal static async Task<byte[]> ReadSourceBody(Stream input, CancellationToken token, TimeSpan? timeout = null)
    {
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(token);
        deadline.CancelAfter(timeout ?? TimeSpan.FromSeconds(20));
        token = deadline.Token;
        using var output = new MemoryStream(); var buffer = new byte[16384];
        while (true) { int count = await input.ReadAsync(buffer, token); if (count == 0) break; if (output.Length + count > MaxSource) throw new InvalidDataException("SourceLink body exceeds budget"); await output.WriteAsync(buffer.AsMemory(0, count), token); }
        return output.ToArray();
    }
}
