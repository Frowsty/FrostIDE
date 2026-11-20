// SPDX-License-Identifier: Apache-2.0
// Optional Test Framework provider contract. No Test Framework assembly dependency in the core editor link.
using System;
using System.Text;
using UnityEngine;
namespace FrostIDE.UnityIntegration {
    public static class UnityTestBridge {
        [Serializable] public sealed class Request {
            public string op, mode, job, owner, catalog, snapshot;
            public int offset;
            public string[] ids;
        }
        [Serializable] public sealed class TestCase {
            public string id, name, fullName, outcome, message, stack;
            public double duration;
            public bool truncated;
        }
        [Serializable] public sealed class Response {
            public int version = 1, total, offset, next;
            public bool ok, available;
            public string message, owner, job, mode, catalog, snapshot, state, execution;
            public TestCase[] cases = new TestCase[0];
        }
        public interface IProvider { Response Handle(Request request); }
        private static IProvider provider;
        public static string ProjectRoot { get { return UnityBrokerClient.PhysicalProject(System.IO.Directory.GetParent(Application.dataPath).FullName); } }
        public static void Register(IProvider value) { if (value == null) throw new ArgumentNullException(nameof(value)); provider = value; }
        public static bool AcceptCommand(string command) { return UnityControlProtocol.TestCommand(command); }
        public static Response Dispatch(string command) {
            try {
                if(!AcceptCommand(command))throw new ArgumentException("Invalid signed Unity test command");
                string encoded=command.Substring(6).Replace('-','+').Replace('_','/');
                encoded=encoded.PadRight((encoded.Length+3)/4*4,'=');
                var bytes=Convert.FromBase64String(encoded);
                string text=new UTF8Encoding(false,true).GetString(bytes);
                int depth=0;bool quoted=false,escaped=false;
                foreach(char c in text){if(quoted){if(escaped)escaped=false;else if(c=='\\')escaped=true;else if(c=='"')quoted=false;continue;}if(c=='"'){quoted=true;continue;}if(c=='{'||c=='['){if(++depth>8)throw new ArgumentException("Unity test JSON nesting exceeds its contract");}else if(c=='}'||c==']'){if(--depth<0)throw new ArgumentException("Unbalanced Unity test payload");}}
                if(depth!=0||quoted||!text.TrimStart().StartsWith("{",StringComparison.Ordinal)||!text.TrimEnd().EndsWith("}",StringComparison.Ordinal))throw new ArgumentException("Unity test payload is not a bounded object");
                var request=JsonUtility.FromJson<Request>(text);
                if(request==null || (request.op!="discover"&&request.op!="run"&&request.op!="snapshot"&&request.op!="page"&&request.op!="cancel"&&request.op!="observe"))throw new ArgumentException("Unknown Unity test operation");
                if(provider==null)return new Response{ok=false,available=false,state="unavailable",message="Install a supported Unity Test Framework package and wait for its editor provider to load"};
                var result=provider.Handle(request);
                if(result==null || Encoding.UTF8.GetByteCount(JsonUtility.ToJson(result))>240*1024)throw new InvalidOperationException("Unity test response exceeded its bounded transport budget");
                return result;
            } catch(Exception error) { string message=error.Message??"Unity test request failed";if(message.Length>512)message=message.Substring(0,512);return new Response{ok=false,available=provider!=null,state="error",message=message}; }
        }
    }
}
