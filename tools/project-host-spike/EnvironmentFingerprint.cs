// SPDX-License-Identifier: Apache-2.0
// Process environment fingerprint for experimental MSBuild edit request lineage.
using System;
using System.Collections;
using System.Linq;
using System.Security.Cryptography;
using System.Text;

static class EnvironmentFingerprint
{
    public static string Current()
    {
        var variables = Environment.GetEnvironmentVariables().Cast<DictionaryEntry>()
            .Select(e => ((string)e.Key).ToUpperInvariant() + "=" + (string)e.Value)
            .OrderBy(s => s, StringComparer.Ordinal);
        using (var sha = SHA256.Create())
            return BitConverter.ToString(sha.ComputeHash(Encoding.UTF8.GetBytes(String.Join("\0", variables)))).Replace("-", "");
    }
}
