// SPDX-License-Identifier: Apache-2.0
// Validate one portable PDB, its PE image and one exact on-disk source document.
using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection.Metadata;
using System.Reflection.PortableExecutable;
using System.Security.Cryptography;
using System.Text.Json;

internal static class Program
{
    private const long MaxFileSize = 256L * 1024 * 1024;
    private static readonly Guid Sha256 = new("8829d00f-11b8-4213-878b-770e8597ac16");

    // The caller must enforce workspace trust, reject dirty buffers and revalidate revisions
    // before starting an adapter. The helper checks the bytes it read, not later filesystem changes.
    private static int Main(string[] args)
    {
        if (args.Length != 3 || !Path.IsPathFullyQualified(args[0]) || !Path.IsPathFullyQualified(args[1]) ||
            (args[2] != "--all" && args[2] != "--manifest" && !Path.IsPathFullyQualified(args[2])))
            return Fail("expected absolute assembly and portable PDB paths and an absolute source path or --all or --manifest");
        try
        {
            var pdbBytes = ReadBounded(args[1]);
            using var pdbStream = new MemoryStream(pdbBytes, writable: false);
            using var provider = MetadataReaderProvider.FromPortablePdbStream(pdbStream);
            var metadata = provider.GetMetadataReader();
            var header = metadata.DebugMetadataHeader ?? throw new BadImageFormatException("missing PDB header");
            var id = new BlobContentId(header.Id);
            int idOffset = header.IdStartOffset;
            if (idOffset < 0 || idOffset > pdbBytes.Length - 20)
                return Fail("invalid portable PDB ID offset");
            var checksumBytes = (byte[])pdbBytes.Clone();
            Array.Clear(checksumBytes, idOffset, 20);
            bool matchedId = false, matchedChecksum = false;
            byte[] peBytes = ReadBounded(args[0]);
            Guid moduleId;
            using (var peStream = new MemoryStream(peBytes, writable: false))
            using (var pe = new PEReader(peStream))
            {
                var peMetadata = pe.GetMetadataReader();
                moduleId = peMetadata.GetGuid(peMetadata.GetModuleDefinition().Mvid);
                if (moduleId == Guid.Empty)
                    return Fail("assembly has no module identity");
                foreach (var entry in pe.ReadDebugDirectory())
                {
                    if (entry.Type == DebugDirectoryEntryType.CodeView)
                    {
                        var codeView = pe.ReadCodeViewDebugDirectoryData(entry);
                        matchedId |= codeView.Guid == id.Guid && entry.Stamp == id.Stamp && codeView.Age == 1;
                    }
                    else if (entry.Type == DebugDirectoryEntryType.PdbChecksum)
                    {
                        var checksum = pe.ReadPdbChecksumDebugDirectoryData(entry);
                        byte[]? hash = checksum.AlgorithmName switch
                        {
                            "SHA256" => SHA256.HashData(checksumBytes),
                            "SHA384" => SHA384.HashData(checksumBytes),
                            "SHA512" => SHA512.HashData(checksumBytes),
                            _ => null,
                        };
                        if (hash != null && CryptographicOperations.FixedTimeEquals(checksum.Checksum.AsSpan(), hash))
                            matchedChecksum = true;
                    }
                }
            }
            if (!matchedId || !matchedChecksum)
                return Fail("assembly and portable PDB do not match");
            bool manifest = args[2] == "--manifest";
            bool allDocuments = args[2] == "--all" || manifest;
            string sourcePath = allDocuments ? "" : Path.GetFullPath(args[2]);
            // Windows can enable case-sensitive directories. Require the PDB's exact spelling
            // until the caller can compare filesystem identities for differently cased aliases.
            const StringComparison comparison = StringComparison.Ordinal;
            var validatedSources = new List<string>();
            var sourceHashes = new List<object>();
            // The IDE's Windows document model compares paths case-insensitively. Refuse
            // two PDB entries it would conflate, even on a case-sensitive Windows directory.
            var seenSources = new HashSet<string>(OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase
                                                                              : StringComparer.Ordinal);
            foreach (var handle in metadata.Documents)
            {
                var document = metadata.GetDocument(handle);
                string name = metadata.GetString(document.Name);
                if (!Path.IsPathFullyQualified(name))
                {
                    if (allDocuments)
                        return Fail("portable PDB contains a non-absolute source document path");
                    continue;
                }
                string documentPath = Path.GetFullPath(name);
                if (!allDocuments && !string.Equals(documentPath, sourcePath, comparison))
                    continue;
                if (!seenSources.Add(documentPath))
                    return Fail("portable PDB contains duplicate source document paths");
                byte[] source = ReadBounded(documentPath);
                Guid algorithm = metadata.GetGuid(document.HashAlgorithm);
                byte[]? actual = algorithm == Sha256 ? SHA256.HashData(source) : null;
                if (actual == null || !CryptographicOperations.FixedTimeEquals(metadata.GetBlobBytes(document.Hash), actual))
                    return Fail("source document checksum does not match portable PDB");
                validatedSources.Add(documentPath);
                sourceHashes.Add(new { path = documentPath, sha256 = Convert.ToHexString(actual).ToLowerInvariant() });
            }
            if (validatedSources.Count == 0 || (!allDocuments && validatedSources.Count != 1))
                return Fail("portable PDB does not identify the required source documents");
            if (manifest)
                Console.WriteLine(JsonSerializer.Serialize(new {
                    moduleId = moduleId.ToString(),
                    assemblySha256 = Convert.ToHexString(SHA256.HashData(peBytes)).ToLowerInvariant(),
                    pdbSha256 = Convert.ToHexString(SHA256.HashData(pdbBytes)).ToLowerInvariant(),
                    sources = sourceHashes,
                }));
            else
                Console.WriteLine(JsonSerializer.Serialize(validatedSources));
            return 0;
        }
        catch (Exception error) when (error is IOException || error is UnauthorizedAccessException ||
                                      error is BadImageFormatException || error is ArgumentException ||
                                      error is InvalidOperationException || error is NotSupportedException)
        {
            return Fail(error.Message);
        }
    }

    private static byte[] ReadBounded(string path)
    {
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        if (stream.Length > MaxFileSize)
            throw new IOException("input exceeds the size limit");
        using var output = new MemoryStream();
        byte[] buffer = new byte[64 * 1024];
        int count;
        while ((count = stream.Read(buffer, 0, buffer.Length)) != 0)
        {
            if (output.Length > MaxFileSize - count)
                throw new IOException("input exceeds the size limit");
            output.Write(buffer, 0, count);
        }
        return output.ToArray();
    }

    private static int Fail(string message)
    {
        Console.Error.WriteLine(message);
        return 1;
    }
}
