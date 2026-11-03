// SPDX-License-Identifier: Apache-2.0
// Original project-scoped Unity editor control service, dispatched only on the editor thread.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using UnityEditor;
using UnityEngine;

namespace FrostIDE.UnityIntegration
{
    [InitializeOnLoad]
    internal static class UnityControlService
    {
        [Serializable] private sealed class Announcement
        {
            public string method = "unity.hello", root, instance, session, token;
            public int version = 2, generation, pid, port;
        }
        [Serializable] private sealed class Challenge { public int version; public string session, challenge; }
        [Serializable] private sealed class Proof { public string session, nonce, proof; }
        [Serializable] private sealed class Request
        {
            public string project, session, nonce, mac, id, command;
            public int version, ttlMs;
        }
        [Serializable] private sealed class Reply
        {
            public string id, session, message;
            public bool ok, playing, paused;
            public int frame;
        }
        private sealed class Pending
        {
            internal Request request;
            internal TcpClient client;
            internal long deadline;
            internal readonly ManualResetEventSlim done = new ManualResetEventSlim();
            internal Reply result;
            internal int cancelled;
            internal int frame;
        }
        private static readonly object gate = new object();
        private static readonly Queue<Pending> queue = new Queue<Pending>();
        private static readonly Stopwatch clock = Stopwatch.StartNew();
        private static TcpListener listener;
        private static UnityControlProtocol protocol;
        private static Pending transition;
        private static string root, instance, session, token, announcement;
        internal static string LastBrokerFailure = String.Empty;
        private static volatile bool alive;
        private static int active;
        private const int MaxConnections = 4;

        static UnityControlService()
        {
            try
            {
                root = UnityBrokerClient.PhysicalProject(Directory.GetParent(Application.dataPath).FullName);
                instance = SessionState.GetString("FrostIDE.UnityControls.Instance", String.Empty);
                if (!UnityControlProtocol.Hex(instance, 32))
                {
                    instance = UnityControlProtocol.NewSecret(16);
                    SessionState.SetString("FrostIDE.UnityControls.Instance", instance);
                    SessionState.SetInt("FrostIDE.UnityControls.Generation", 0);
                }
                int previous = SessionState.GetInt("FrostIDE.UnityControls.Generation", 0);
                if (previous < 0 || previous == Int32.MaxValue)
                {
                    instance = UnityControlProtocol.NewSecret(16);
                    SessionState.SetString("FrostIDE.UnityControls.Instance", instance);
                    previous = 0;
                }
                int generation = previous + 1;
                SessionState.SetInt("FrostIDE.UnityControls.Generation", generation);
                session = UnityControlProtocol.NewSecret(16);
                token = UnityControlProtocol.NewSecret(32);
                protocol = new UnityControlProtocol(root, session, token);
                listener = new TcpListener(IPAddress.Loopback, 0);
                listener.Start(MaxConnections);
                int port = ((IPEndPoint)listener.LocalEndpoint).Port;
                announcement = JsonUtility.ToJson(new Announcement { root = root, instance = instance, generation = generation,
                    session = session, token = token, pid = Process.GetCurrentProcess().Id, port = port });
                alive = true;
                EditorApplication.update += Update;
                AssemblyReloadEvents.beforeAssemblyReload += Shutdown;
                EditorApplication.quitting += Shutdown;
                var thread = new Thread(Listen) { IsBackground = true, Name = "FrostIDE Unity control" };
                thread.Start();
            }
            catch (Exception error) { UnityEngine.Debug.LogError("FrostIDE Unity control unavailable: " + error.Message); Shutdown(); }
        }
        private static void Listen()
        {
            long next = 0;
            while (alive)
            {
                try
                {
                    if (clock.ElapsedMilliseconds >= next)
                    {
                        next = clock.ElapsedMilliseconds + 2000;
                        try { UnityBrokerClient.Announce(root, announcement, () => !alive); lock (gate) LastBrokerFailure = String.Empty; }
                        catch (UnityBrokerClient.BrokerFailure error) { lock (gate) LastBrokerFailure = error.Stage + ":" + error.Kind; }
                        catch (Exception error)
                        {
                            string kind = error.GetType().Name;
                            lock (gate) LastBrokerFailure = "unexpected:" + (kind.Length <= 64 ? kind : kind.Substring(0, 64));
                        }
                    }
                    if (!listener.Pending()) { Thread.Sleep(30); continue; }
                    TcpClient client = listener.AcceptTcpClient();
                    if (Interlocked.Increment(ref active) > MaxConnections)
                    { Interlocked.Decrement(ref active); client.Close(); continue; }
                    ThreadPool.QueueUserWorkItem(_ => Serve(client));
                }
                catch (Exception) { if (alive) Thread.Sleep(50); }
            }
        }
        private static string ReadLine(TcpClient client, NetworkStream stream)
        {
            var bytes = new List<byte>();
            long deadline = clock.ElapsedMilliseconds + 2000;
            while (bytes.Count <= 8192)
            {
                long left = deadline - clock.ElapsedMilliseconds;
                if (left <= 0) throw new IOException("Unity request read timed out");
                client.ReceiveTimeout = (int)left;
                int value = stream.ReadByte();
                if (value < 0) throw new IOException("Unity client closed");
                if (value == '\n') return new UTF8Encoding(false, true).GetString(bytes.ToArray());
                bytes.Add((byte)value);
            }
            throw new IOException("Unity request too large");
        }
        private static bool Disconnected(TcpClient client)
        {
            try { return client.Client.Poll(0, SelectMode.SelectRead) && client.Available == 0; }
            catch (Exception) { return true; }
        }
        private static void Serve(TcpClient client)
        {
            Pending pending = null;
            try
            {
                client.ReceiveTimeout = 2000; client.SendTimeout = 2000;
                using (client)
                using (NetworkStream stream = client.GetStream())
                {
                    Challenge hello = JsonUtility.FromJson<Challenge>(ReadLine(client, stream));
                    var connection = hello == null ? null : protocol.Open(hello.version, hello.session, hello.challenge);
                    if (connection == null) throw new IOException("Unity challenge refused");
                    byte[] proofBytes = Encoding.UTF8.GetBytes(JsonUtility.ToJson(new Proof { session = session,
                        nonce = connection.ServerNonce, proof = connection.Proof }) + "\n");
                    stream.Write(proofBytes, 0, proofBytes.Length);
                    Request request = JsonUtility.FromJson<Request>(ReadLine(client, stream));
                    long now = clock.ElapsedMilliseconds;
                    if (request == null || !connection.TryReserve(request.version, request.project, request.session,
                        request.id, request.command, request.ttlMs, request.nonce, request.mac))
                        throw new IOException("Unity request refused");
                    pending = new Pending { request = request, client = client, deadline = now + request.ttlMs };
                    lock (gate) { if (!alive || queue.Count >= 32) throw new IOException("Unity request unavailable"); queue.Enqueue(pending); }
                    if (!pending.done.Wait(request.ttlMs)) { Interlocked.Exchange(ref pending.cancelled, 1); return; }
                    byte[] reply = Encoding.UTF8.GetBytes(JsonUtility.ToJson(pending.result) + "\n");
                    stream.Write(reply, 0, reply.Length);
                }
            }
            catch (Exception) { }
            finally { if (pending != null) Interlocked.Exchange(ref pending.cancelled, 1); Interlocked.Decrement(ref active); }
        }
        private static void Finish(Pending item, bool ok, string message)
        {
            item.result = new Reply { id = item.request.id, session = session, ok = ok, message = message,
                playing = EditorApplication.isPlaying, paused = EditorApplication.isPaused, frame = Time.frameCount };
            item.done.Set();
        }
        private static bool Expired(Pending item)
        {
            return !alive || Interlocked.CompareExchange(ref item.cancelled, 0, 0) != 0 ||
                   clock.ElapsedMilliseconds >= item.deadline || Disconnected(item.client);
        }
        private static void Update()
        {
            if (!alive) return;
            if (transition != null)
            {
                Pending item = transition;
                if (Expired(item)) { transition = null; Finish(item, false, "Request expired"); }
                else
                {
                    string command = item.request.command;
                    bool observed = command == "play" ? EditorApplication.isPlaying :
                        command == "stop" ? !EditorApplication.isPlaying :
                        command == "pause" ? EditorApplication.isPaused :
                        command == "resume" ? !EditorApplication.isPaused : Time.frameCount > item.frame;
                    if (observed) { transition = null; Finish(item, true, "State observed"); }
                }
            }
            for (int processed = 0; processed < 8; ++processed)
            {
                Pending item;
                lock (gate) { if (queue.Count == 0) break; item = queue.Dequeue(); }
                if (Expired(item)) { Finish(item, false, "Request expired"); continue; }
                if (item.request.command == "state") { Finish(item, true, "State observed"); continue; }
                if (transition != null) { Finish(item, false, "Another control request is pending"); continue; }
                string command = item.request.command;
                if ((command == "pause" || command == "resume" || command == "step") && !EditorApplication.isPlaying)
                { Finish(item, false, "Unity is not playing"); continue; }
                if (command == "step" && !EditorApplication.isPaused)
                { Finish(item, false, "Pause Unity before stepping"); continue; }
                item.frame = Time.frameCount;
                transition = item;
                if (command == "play") EditorApplication.isPlaying = true;
                else if (command == "stop") EditorApplication.isPlaying = false;
                else if (command == "pause") EditorApplication.isPaused = true;
                else if (command == "resume") EditorApplication.isPaused = false;
                else EditorApplication.Step();
            }
        }
        private static void Shutdown()
        {
            alive = false;
            protocol?.Close();
            try { listener?.Stop(); } catch (Exception) { }
            EditorApplication.update -= Update;
            AssemblyReloadEvents.beforeAssemblyReload -= Shutdown;
            EditorApplication.quitting -= Shutdown;
            lock (gate) while (queue.Count > 0) { var item = queue.Dequeue(); Interlocked.Exchange(ref item.cancelled, 1); item.done.Set(); }
            if (transition != null) { Interlocked.Exchange(ref transition.cancelled, 1); transition.done.Set(); transition = null; }
        }
    }
}
