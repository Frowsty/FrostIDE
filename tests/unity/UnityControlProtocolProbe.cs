// SPDX-License-Identifier: Apache-2.0
// Exercise one-shot Unity control authentication, transcript binding and long sessions.
using System;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using FrostIDE.UnityIntegration;
internal static class UnityControlProtocolProbe
{
    private static string Sign(string secret, params string[] fields)
    {
        using (var stream = new MemoryStream())
        {
            foreach (string field in fields)
            {
                byte[] bytes = Encoding.UTF8.GetBytes(field); uint length = (uint)bytes.Length;
                stream.WriteByte((byte)(length >> 24)); stream.WriteByte((byte)(length >> 16));
                stream.WriteByte((byte)(length >> 8)); stream.WriteByte((byte)length); stream.Write(bytes, 0, bytes.Length);
            }
            using (var hmac = new HMACSHA256(Encoding.ASCII.GetBytes(secret)))
            {
                var value = new StringBuilder(); foreach (byte b in hmac.ComputeHash(stream.ToArray())) value.Append(b.ToString("x2")); return value.ToString();
            }
        }
    }
    private static void Main()
    {
        string session = new string('a', 32), token = new string('b', 64), id = new string('c', 32), challenge = new string('d', 64);
        var protocol = new UnityControlProtocol("/physical/project", session, token);
        var first = protocol.Open(2, session, challenge) ?? throw new Exception("connection rejected");
        if (first.Proof != Sign(token, "frost-unity-server-v2", "/physical/project", session, challenge, first.ServerNonce)) throw new Exception("server proof");
        string mac = Sign(token, "frost-unity-request-v2", "/physical/project", session, id, "play", "100", challenge, first.ServerNonce);
        if (!first.TryReserve(2, "/physical/project", session, id, "play", 100, first.ServerNonce, mac)) throw new Exception("valid request rejected");
        if (first.TryReserve(2, "/physical/project", session, id, "play", 100, first.ServerNonce, mac)) throw new Exception("same connection replay");
        var replay = protocol.Open(2, session, challenge);
        if (replay.TryReserve(2, "/physical/project", session, id, "play", 100, first.ServerNonce, mac)) throw new Exception("captured replay accepted");
        for (int i = 0; i < 4100; ++i)
        {
            var connection = protocol.Open(2, session, challenge);
            string signature = Sign(token, "frost-unity-request-v2", "/physical/project", session, id, "state", "100", challenge, connection.ServerNonce);
            if (!connection.TryReserve(2, "/physical/project", session, id, "state", 100, connection.ServerNonce, signature))
                throw new Exception("fresh connection refused at " + i);
        }
        var changed = protocol.Open(2, session, challenge);
        string valid = Sign(token, "frost-unity-request-v2", "/physical/project", session, id, "play", "100", challenge, changed.ServerNonce);
        if (changed.TryReserve(2, "/other", session, id, "play", 100, changed.ServerNonce, valid)) throw new Exception("wrong project accepted");
        foreach (string field in new[] { "session", "id", "command", "ttl", "nonce", "challenge", "mac", "expired", "too-long" })
        {
            var connection = protocol.Open(2, session, challenge);
            string signature = Sign(token, "frost-unity-request-v2", "/physical/project", session, id, "play", "100", challenge, connection.ServerNonce);
            string valueSession = field == "session" ? new string('e', 32) : session;
            string valueId = field == "id" ? new string('f', 32) : id;
            string valueCommand = field == "command" ? "stop" : "play";
            int valueTtl = field == "ttl" ? 101 : field == "expired" ? 0 : field == "too-long" ? 5001 : 100;
            string valueNonce = field == "nonce" ? new string('a', 64) : connection.ServerNonce;
            if (field == "challenge") signature = Sign(token, "frost-unity-request-v2", "/physical/project", session, id, "play", "100", new string('e', 64), connection.ServerNonce);
            if (field == "mac") signature = new string('0', 64);
            if (connection.TryReserve(2, "/physical/project", valueSession, valueId, valueCommand, valueTtl, valueNonce, signature))
                throw new Exception("modified " + field + " accepted");
        }
        protocol.Close();
        if (protocol.Open(2, session, challenge) != null) throw new Exception("closed session accepted");
        Console.WriteLine("Unity protocol: transcript, one-shot replay and 4100 fresh requests passed");
    }
}
