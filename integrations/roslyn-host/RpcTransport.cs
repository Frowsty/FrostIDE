// SPDX-License-Identifier: Apache-2.0
// Bounded LSP framing; a single actor orders workspace edits while the reader admits cancellation.
using System.Collections.Concurrent;
using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Threading.Channels;
namespace Frost.RoslynHost;
internal sealed class RpcTransport
{
    internal const int MaximumMessage = 16 * 1024 * 1024;
    private readonly Stream input = Console.OpenStandardInput(), output = Console.OpenStandardOutput();
    private readonly SemaphoreSlim writer = new(1, 1);
    private readonly Channel<JsonElement> queue = Channel.CreateBounded<JsonElement>(new BoundedChannelOptions(128) { SingleReader = true, SingleWriter = true });
    private readonly ConcurrentDictionary<string, CancellationTokenSource> requests = new();
    internal async Task Send(object message)
    {
        var bytes = JsonSerializer.SerializeToUtf8Bytes(message);
        if (bytes.Length > MaximumMessage) throw new InvalidDataException("LSP response exceeds 16 MiB");
        await writer.WaitAsync();
        try { await output.WriteAsync(Encoding.ASCII.GetBytes($"Content-Length: {bytes.Length}\r\n\r\n")); await output.WriteAsync(bytes); await output.FlushAsync(); }
        finally { writer.Release(); }
    }
    internal Task Notify(string method, object value) => Send(new { jsonrpc = "2.0", method, @params = value });
    internal async Task Read(CancellationToken stop)
    {
        try {
            while (!stop.IsCancellationRequested) {
                var header = new List<byte>(); int suffix = 0;
                while (suffix != 4) {
                    var one = new byte[1];
                    if (await input.ReadAsync(one, stop) == 0) { if (header.Count == 0) return; throw new EndOfStreamException(); }
                    header.Add(one[0]); if (header.Count > 8192) throw new InvalidDataException("LSP header exceeds 8 KiB");
                    suffix = (suffix, one[0]) switch { (0, 13) => 1, (1, 10) => 2, (2, 13) => 3, (3, 10) => 4, (_, 13) => 1, _ => 0 };
                }
                int length = -1;
                foreach (var line in Encoding.ASCII.GetString(header.ToArray()).Split("\r\n", StringSplitOptions.RemoveEmptyEntries)) {
                    var split = line.IndexOf(':');
                    if (split < 0) throw new InvalidDataException("Invalid LSP header");
                    if (line[..split].Equals("Content-Length", StringComparison.OrdinalIgnoreCase)) {
                        if (length != -1 || !int.TryParse(line[(split + 1)..].Trim(), NumberStyles.None, CultureInfo.InvariantCulture, out length)) throw new InvalidDataException("Invalid LSP length");
                    }
                }
                if (length < 1 || length > MaximumMessage) throw new InvalidDataException("LSP payload budget exceeded");
                var payload = new byte[length]; await input.ReadExactlyAsync(payload, stop);
                using var document = JsonDocument.Parse(payload, new JsonDocumentOptions { MaxDepth = 64 });
                var message = document.RootElement.Clone();
                if (message.TryGetProperty("method", out var method) && method.GetString() == "$/cancelRequest") {
                    var id = message.GetProperty("params").GetProperty("id").GetRawText();
                    if (requests.TryGetValue(id, out var request)) request.Cancel();
                    continue;
                }
                if (message.TryGetProperty("id", out var requestId) && message.TryGetProperty("method", out _)) {
                    if (!requests.TryAdd(requestId.GetRawText(), CancellationTokenSource.CreateLinkedTokenSource(stop))) throw new InvalidDataException("Duplicate live LSP request identity");
                }
                if (!queue.Writer.TryWrite(message)) throw new InvalidDataException("RoslynHost request backlog exceeds 128 messages");
            }
        } finally { queue.Writer.TryComplete(); }
    }
    internal async Task Consume(Func<JsonElement, CancellationToken, Task<object?>> dispatch, CancellationToken stop)
    {
        await foreach (var message in queue.Reader.ReadAllAsync(stop)) {
            if (!message.TryGetProperty("method", out _)) continue; // Responses to server notifications need no workspace action.
            var hasId = message.TryGetProperty("id", out var id);
            var token = hasId && requests.TryGetValue(id.GetRawText(), out var source) ? source.Token : stop;
            try {
                token.ThrowIfCancellationRequested();
                var result = await dispatch(message, token);
                if (hasId) await Send(new { jsonrpc = "2.0", id, result });
            } catch (Exception error) {
                if (hasId) await Send(new { jsonrpc = "2.0", id, error = new { code = error is OperationCanceledException ? -32800 : error is NotSupportedException ? -32601 : -32603, message = error.Message } });
                else await Notify("window/logMessage", new { type = 1, message = error.Message });
            } finally { if (hasId && requests.TryRemove(id.GetRawText(), out var completed)) completed.Dispose(); }
        }
    }
}
