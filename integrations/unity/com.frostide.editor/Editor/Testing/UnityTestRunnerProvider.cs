// SPDX-License-Identifier: Apache-2.0
// Real public TestRunnerApi callbacks. Serialized editor-only ownership survives a PlayMode domain reload.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using UnityEditor;
using UnityEditor.Compilation;
using UnityEditor.TestTools.TestRunner.Api;
using UnityEngine;
namespace FrostIDE.UnityIntegration {
    [InitializeOnLoad]
    internal sealed class UnityTestRunnerProvider : ScriptableObject, UnityTestBridge.IProvider, ICallbacks {
        private const int MaxCases=2048;
        [SerializeField] private string projectRoot, owner, job, catalog, mode, state="idle", execution, message;
        [SerializeField] private bool running, ownStarted, foreignRunning, uncertain, cancelling;
        [SerializeField] private TestRunnerApi api;
        [SerializeField] private List<UnityTestBridge.TestCase> rows=new List<UnityTestBridge.TestCase>();
        [NonSerialized] private string snapshot;
        [NonSerialized] private List<UnityTestBridge.TestCase> exported=new List<UnityTestBridge.TestCase>();
        [NonSerialized] private string exportedState, exportedMessage, exportedJob, exportedMode, exportedCatalog, exportedExecution;
        static UnityTestRunnerProvider() {
            try {
                string root=UnityTestBridge.ProjectRoot;
                var instances=Resources.FindObjectsOfTypeAll<UnityTestRunnerProvider>().Where(item=>item.projectRoot==root).ToArray();
                if(instances.Length>1)throw new InvalidOperationException("Multiple Unity test observers claim this project");
                var instance=instances.Length==1?instances[0]:CreateInstance<UnityTestRunnerProvider>();
                if(instances.Length==0){instance.projectRoot=root;instance.owner=Guid.NewGuid().ToString("N");instance.hideFlags=HideFlags.HideAndDontSave;}
                instance.Initialize();
            }catch(Exception error){Debug.LogError("FrostIDE test provider unavailable: "+error.Message);}
        }
        private void Initialize(){
            if(api==null)api=CreateInstance<TestRunnerApi>();
            api.UnregisterCallbacks(this);api.RegisterCallbacks(this);
            exported=new List<UnityTestBridge.TestCase>();snapshot=null;
            UnityTestBridge.Register(this);
            CompilationPipeline.compilationStarted -= CompilationStarted;
            CompilationPipeline.compilationStarted += CompilationStarted;
        }
        private void CompilationStarted(object context){
            catalog=null;
            if(state=="discovering"){
                state="cancelled";
                message="Compilation invalidated the pending Unity test discovery";
            }
            if(running){
                uncertain=true;
                state="uncertain";
                message="Compilation changed the active test context; results are not complete";
            }
        }
        private static bool Token(string value){if(value==null||value.Length!=32)return false;foreach(char c in value)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;return true;}
        private static string Identity(string value,int bytes){if(String.IsNullOrEmpty(value)||Encoding.UTF8.GetByteCount(value)>bytes)throw new InvalidOperationException("Test identity exceeds the supported observation profile");return value;}
        private static string Clip(string value,int limit,out bool truncated){
            value=value??String.Empty;int low=0,high=Math.Min(value.Length,limit);
            while(low<high){int middle=low+(high-low+1)/2;if(Encoding.UTF8.GetByteCount(value.Substring(0,middle))<=limit)low=middle;else high=middle-1;}
            if(low>0&&Char.IsHighSurrogate(value[low-1]))--low;truncated=low<value.Length;return truncated?value.Substring(0,low):value;
        }
        private static UnityTestBridge.TestCase Copy(UnityTestBridge.TestCase row){return new UnityTestBridge.TestCase{id=row.id,name=row.name,fullName=row.fullName,outcome=row.outcome,message=row.message,stack=row.stack,duration=row.duration,truncated=row.truncated};}
        private void Walk(ITestAdaptor node,List<UnityTestBridge.TestCase> found,ref int visited,int depth){
            if(++visited>8192||depth>64)throw new InvalidOperationException("Unity test tree exceeds its bounded structural profile");
            if(node.IsSuite){foreach(var child in node.Children)Walk(child,found,ref visited,depth+1);return;}
            if(found.Count>=MaxCases)throw new InvalidOperationException("Unity test catalog exceeds 2048 cases; no partial executable catalog was published");
            var id=Identity(node.UniqueName,512);if(found.Any(item=>item.id==id))throw new InvalidOperationException("Unity returned a duplicate test identity");
            found.Add(new UnityTestBridge.TestCase{id=id,name=Identity(node.Name,256),fullName=Identity(node.FullName,1024),outcome="Discovered",message="",stack=""});
        }
        private string WireExecution(){
            Guid id;
            return Guid.TryParse(execution,out id)&&id!=Guid.Empty?id.ToString("D"):"";
        }
        private UnityTestBridge.Response Result(bool ok,string text){bool clipped;string detail=Clip(text??message??"",2048,out clipped);return new UnityTestBridge.Response{ok=ok,available=true,message=detail,owner=owner,job=job??"",catalog=catalog??"",mode=mode??"",state=state??"idle",execution=WireExecution()};}
        private void RequireOwner(UnityTestBridge.Request request){if(request.owner!=owner||request.job!=job)throw new InvalidOperationException("Unity test owner/job changed; observe the actual editor state before acting");}
        public UnityTestBridge.Response Handle(UnityTestBridge.Request request){
            if(projectRoot!=UnityTestBridge.ProjectRoot)throw new InvalidOperationException("Unity project ownership changed");
            if(request.op=="observe")return Result(true,message);
            if(request.op=="page"){
                RequireOwner(request);if(!Token(snapshot)||request.snapshot!=snapshot||request.offset<0||request.offset>exported.Count)throw new InvalidOperationException("Unity test snapshot expired");return Page(request.offset);
            }
            if(request.op=="snapshot"){
                RequireOwner(request);exported=rows.Select(Copy).ToList();snapshot=Guid.NewGuid().ToString("N");exportedState=state;exportedMessage=message;exportedJob=job;exportedMode=mode;exportedCatalog=catalog;exportedExecution=WireExecution();return Page(0);
            }
            if(request.op=="cancel"){
                RequireOwner(request);
                if(state=="discovering"){state="cancelled";message="Discovery observation cancelled";return Result(true,message);}
                if(!running||String.IsNullOrEmpty(execution))return Result(false,"No owned TestRunnerApi execution can be cancelled");
                if(!TestRunnerApi.CancelTestRun(execution)){uncertain=true;state="uncertain";return Result(false,"TestRunnerApi did not confirm cancellation; observe completion before rerunning");}
                cancelling=true;state="cancelling";return Result(true,"Cancellation accepted; waiting for real RunFinished callbacks");
            }
            if(running||foreignRunning||state=="discovering")throw new InvalidOperationException("A Unity test run or discovery is already active");
            if(EditorApplication.isCompiling||EditorApplication.isPlayingOrWillChangePlaymode)throw new InvalidOperationException("Stop ordinary play mode and wait for compilation before starting tests");
            if(request.mode!="EditMode"&&request.mode!="PlayMode")throw new ArgumentException("Select EditMode or PlayMode");
            if(!Token(request.job)||request.job==job)throw new ArgumentException("A fresh test operation identity is required");
            var testMode=request.mode=="EditMode"?TestMode.EditMode:TestMode.PlayMode;
            if(request.op=="discover"){
                job=request.job;mode=request.mode;execution=null;catalog=null;state="discovering";message="Retrieving the actual Unity test tree";rows.Clear();string expected=job;
                api.RetrieveTestList(testMode,root=>{if(job!=expected||state!="discovering")return;try{var found=new List<UnityTestBridge.TestCase>();int visited=0;Walk(root,found,ref visited,0);rows=found;catalog=expected;state="discovered";message="Actual Unity test catalog available";}catch(Exception error){catalog=null;state="error";message=error.Message;}});
                return Result(true,message);
            }
            if(request.op!="run")throw new ArgumentException("Unsupported Unity test operation");
            if(request.owner!=owner||String.IsNullOrEmpty(catalog)||request.catalog!=catalog||request.mode!=mode)throw new InvalidOperationException("Rediscover this exact editor/mode before running tests");
            var selected=request.ids??new string[0];if(selected.Length>64||selected.Distinct().Count()!=selected.Length)throw new ArgumentException("Select at most 64 distinct actual test identities");
            var names=new List<string>();foreach(string id in selected){var row=rows.SingleOrDefault(item=>item.id==id);if(row==null||rows.Count(item=>item.fullName==row.fullName)!=1)throw new InvalidOperationException("Selected test name is absent or ambiguous across assemblies");names.Add(row.fullName);}
            job=request.job;execution=null;running=true;ownStarted=false;uncertain=false;cancelling=false;state="queued";message="TestRunnerApi execution requested";
            foreach(var row in rows){row.outcome="Pending";row.message="";row.stack="";row.duration=0;row.truncated=false;}
            try{execution=api.Execute(new ExecutionSettings(new Filter{testMode=testMode,testNames=names.Count==0?null:names.ToArray()}));if(String.IsNullOrEmpty(WireExecution())){uncertain=true;state="uncertain";message="TestRunnerApi did not return an execution identity";}}
            catch(Exception error){running=false;state="error";message=error.Message;}
            return Result(!uncertain&&state!="error",message);
        }
        private UnityTestBridge.Response Page(int offset){
            int count=Math.Min(8,exported.Count-offset);var result=Result(true,exportedMessage);result.snapshot=snapshot;result.job=exportedJob;result.mode=exportedMode;result.catalog=exportedCatalog??"";result.execution=exportedExecution;result.state=exportedState;result.total=exported.Count;result.offset=offset;result.next=offset+count;result.cases=exported.Skip(offset).Take(count).Select(Copy).ToArray();return result;
        }
        public void RunStarted(ITestAdaptor tests){
            if(!running){foreignRunning=true;return;}
            if(ownStarted){uncertain=true;state="uncertain";message="Overlapping native Unity runs invalidate owned result attribution";return;}
            ownStarted=true;state="running";message="Unity reported RunStarted";
        }
        public void TestStarted(ITestAdaptor test){if(!running||!ownStarted||uncertain||test.IsSuite)return;var row=rows.SingleOrDefault(item=>item.id==test.UniqueName);if(row==null||row.fullName!=test.FullName){uncertain=true;state="uncertain";return;}row.outcome="Running";}
        private void Record(ITestResultAdaptor result,ref int visited,int depth){
            if(++visited>8192||depth>64)throw new InvalidOperationException("Unity test result tree exceeds its bounded structural profile");
            if(result.Test.IsSuite){foreach(var child in result.Children)Record(child,ref visited,depth+1);return;}
            var row=rows.SingleOrDefault(item=>item.id==result.Test.UniqueName);if(row==null||row.fullName!=result.Test.FullName)throw new InvalidOperationException("Framework result identity differs from the owned catalog");
            if(Double.IsNaN(result.Duration)||Double.IsInfinity(result.Duration)||result.Duration<0)throw new InvalidOperationException("Framework reported an invalid test duration");
            row.outcome=result.TestStatus.ToString();row.duration=result.Duration;bool clippedMessage,clippedStack;row.message=Clip(result.Message,512,out clippedMessage);row.stack=Clip(result.StackTrace,2048,out clippedStack);row.truncated=clippedMessage||clippedStack;
        }
        public void TestFinished(ITestResultAdaptor result){if(!running||!ownStarted||uncertain)return;try{int visited=0;Record(result,ref visited,0);}catch(Exception error){uncertain=true;state="uncertain";message=error.Message;}}
        public void RunFinished(ITestResultAdaptor result){
            foreignRunning=false;if(!running)return;
            if(!ownStarted){uncertain=true;state="uncertain";message="RunFinished arrived before the owned RunStarted; execution attribution requires native cleanup";return;}
            if(uncertain){state="uncertain";message="Overlapping or changed test context cannot acknowledge this execution; stop it explicitly in Unity";return;}
            if(!uncertain){try{int visited=0;Record(result,ref visited,0);state="finished";message=cancelling?"Framework RunFinished observed after cancellation":"Framework RunFinished observed";}catch(Exception error){uncertain=true;state="uncertain";message=error.Message;}}
            running=false;ownStarted=false;
        }
    }
}
