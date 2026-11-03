// SPDX-License-Identifier: Apache-2.0
// One-shot authenticated Unity editor commands bound to fresh connection challenges.
using System;
using System.Globalization;
using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace FrostIDE.UnityIntegration
{
    internal sealed class UnityControlProtocol
    {
        private readonly string project, session;
        private readonly byte[] secret;
        private readonly object gate = new object();
        private bool closed;
        internal UnityControlProtocol(string project, string session, string secret)
        {
            if (String.IsNullOrEmpty(project) || !Hex(session, 32) || !Hex(secret, 64))
                throw new ArgumentException("Invalid Unity session identity");
            this.project = project; this.session = session; this.secret = Encoding.ASCII.GetBytes(secret);
        }
        internal static bool Hex(string value, int length)
        {
            if (value == null || value.Length != length) return false;
            foreach (char c in value) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
            return true;
        }
        internal static string NewSecret(int bytes)
        {
            if (bytes != 16 && bytes != 32) throw new ArgumentOutOfRangeException(nameof(bytes));
            var data = new byte[bytes];
            using (var random = RandomNumberGenerator.Create()) random.GetBytes(data);
            var text = new StringBuilder(bytes * 2);
            foreach (byte value in data) text.Append(value.ToString("x2"));
            return text.ToString();
        }
        // Every UTF-8 field is prefixed with its four-byte unsigned big-endian length.
        internal static byte[] Transcript(params string[] fields)
        {
            using (var output = new MemoryStream())
            {
                foreach (string field in fields)
                {
                    byte[] bytes = new UTF8Encoding(false, true).GetBytes(field);
                    uint length = (uint)bytes.Length;
                    output.WriteByte((byte)(length >> 24)); output.WriteByte((byte)(length >> 16));
                    output.WriteByte((byte)(length >> 8)); output.WriteByte((byte)length);
                    output.Write(bytes, 0, bytes.Length);
                }
                return output.ToArray();
            }
        }
        private string Mac(params string[] fields)
        {
            using (var hmac = new HMACSHA256(secret))
            {
                var text = new StringBuilder(64);
                foreach (byte value in hmac.ComputeHash(Transcript(fields))) text.Append(value.ToString("x2"));
                return text.ToString();
            }
        }
        internal sealed class Connection
        {
            private readonly UnityControlProtocol owner;
            internal readonly string ClientChallenge, ServerNonce, Proof;
            private bool consumed;
            internal Connection(UnityControlProtocol owner, string challenge)
            {
                this.owner = owner; ClientChallenge = challenge; ServerNonce = NewSecret(32);
                Proof = owner.Mac("frost-unity-server-v2", owner.project, owner.session, challenge, ServerNonce);
            }
            // Consume before queuing, including malformed or failed requests.
            internal bool TryReserve(int version, string project, string session, string id, string command,
                                     int ttlMs, string nonce, string supplied)
            {
                lock (owner.gate)
                {
                    if (consumed) return false;
                    consumed = true;
                    if (owner.closed || version != 2 || !String.Equals(owner.project, project, StringComparison.Ordinal) ||
                        !String.Equals(owner.session, session, StringComparison.Ordinal) || !Hex(id, 32) ||
                        ttlMs < 1 || ttlMs > 5000 || !String.Equals(ServerNonce, nonce, StringComparison.Ordinal) ||
                        !Hex(supplied, 64) || !(command == "state" || command == "play" || command == "pause" ||
                        command == "resume" || command == "step" || command == "stop")) return false;
                    string expected = owner.Mac("frost-unity-request-v2", project, session, id, command,
                        ttlMs.ToString(CultureInfo.InvariantCulture), ClientChallenge, ServerNonce);
                    int difference = 0;
                    for (int i = 0; i < expected.Length; ++i) difference |= expected[i] ^ supplied[i];
                    return difference == 0;
                }
            }
        }
        internal Connection Open(int version, string requestedSession, string challenge)
        {
            lock (gate)
            {
                if (closed || version != 2 || !String.Equals(session, requestedSession, StringComparison.Ordinal) ||
                    !Hex(challenge, 64)) return null;
                return new Connection(this, challenge);
            }
        }
        internal void Close() { lock (gate) closed = true; }
    }
}
