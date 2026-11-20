// SPDX-License-Identifier: Apache-2.0
// Exercises the actual producer against explicit TestRunnerApi doubles; no claim of Unity runtime execution.
using System;
using System.IO;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Text;
using FrostIDE.UnityIntegration;
using UnityEditor.Compilation;
using UnityEditor.TestTools.TestRunner.Api;
internal static class UnityTestsProbe {
    static void Check(bool value,string reason){if(!value)throw new Exception(reason);}
    static UnityTestBridge.Response Call(UnityTestBridge.Request request){string command="tests:"+Convert.ToBase64String(Encoding.UTF8.GetBytes(UnityEngine.JsonUtility.ToJson(request))).TrimEnd('=').Replace('+','-').Replace('/','_');return UnityTestBridge.Dispatch(command);}
    static void Main(string[] args){
        if(args.Length!=1||!Path.IsPathFullyQualified(args[0]))throw new Exception("private absolute test root required");Directory.CreateDirectory(Path.Combine(args[0],"Assets"));UnityEngine.Application.dataPath=Path.Combine(args[0],"Assets");
        var leaves=Enumerable.Range(0,10).Select(i=>(ITestAdaptor)new FakeTest{UniqueName="Tests.dll/Fixture/Case"+i,Name="Case"+i,FullName="Fixture.Case"+i}).ToArray();
        var root=new FakeTest{IsSuite=true,UniqueName="root",Name="root",FullName="root",Children=leaves};TestRunnerApi.Tree=root;
        RuntimeHelpers.RunClassConstructor(typeof(UnityTestRunnerProvider).TypeHandle);
        var observed=Call(new UnityTestBridge.Request{op="observe"});Check(observed.ok&&observed.available,"actual provider unavailable");
        string discovery=new string('a',32);var discovered=Call(new UnityTestBridge.Request{op="discover",mode="EditMode",job=discovery});Check(discovered.ok&&discovered.catalog==discovery,"catalog not actually discovered");
        var first=Call(new UnityTestBridge.Request{op="snapshot",owner=discovered.owner,job=discovery});Check(first.ok&&first.total==10&&first.cases.Length==8&&first.next==8,"bounded first page");
        var second=Call(new UnityTestBridge.Request{op="page",owner=discovered.owner,job=discovery,snapshot=first.snapshot,offset=first.next});Check(second.ok&&second.offset==8&&second.cases.Length==2,"actual continuation");
        var unknown=Call(new UnityTestBridge.Request{op="run",owner=discovered.owner,catalog=discovery,job=new string('b',32),mode="EditMode",ids=new[]{"invented"}});Check(!unknown.ok&&TestRunnerApi.Last.executed==null,"invented selection executed");
        var run=Call(new UnityTestBridge.Request{op="run",owner=discovered.owner,catalog=discovery,job=new string('c',32),mode="EditMode",ids=new[]{leaves[0].UniqueName}});Check(run.ok&&TestRunnerApi.Last.executed.filter.testNames.SequenceEqual(new[]{leaves[0].FullName}),"actual selection was broadened");
        Check(run.execution==Guid.Parse(TestRunnerApi.Last.execution).ToString("D"),"wire GUID was not canonical");
        var before=Call(new UnityTestBridge.Request{op="snapshot",owner=run.owner,job=run.job});TestRunnerApi.Last.callbacks.RunStarted(root);TestRunnerApi.Last.callbacks.TestStarted(leaves[0]);TestRunnerApi.Last.callbacks.TestFinished(new FakeResult{Test=leaves[0],TestStatus=TestStatus.Failed,Duration=0.25,Message=new string('x',5000),StackTrace=new string('y',5000)});
        var frozen=Call(new UnityTestBridge.Request{op="page",owner=run.owner,job=run.job,snapshot=before.snapshot,offset=8});Check(frozen.state==before.state,"snapshot changed during callbacks");
        var after=Call(new UnityTestBridge.Request{op="snapshot",owner=run.owner,job=run.job});Check(after.cases[0].outcome=="Failed"&&after.cases[0].truncated&&Encoding.UTF8.GetByteCount(after.cases[0].message)<=512&&Encoding.UTF8.GetByteCount(after.cases[0].stack)<=2048,"callback data fabricated or unbounded");
        var cancelled=Call(new UnityTestBridge.Request{op="cancel",owner=run.owner,job=run.job});Check(cancelled.ok&&cancelled.state=="cancelling"&&TestRunnerApi.Last.cancelAccepted,"cancel not acknowledged by API");
        var result=new FakeResult{Test=root,Children=new[]{new FakeResult{Test=leaves[0],TestStatus=TestStatus.Failed,Duration=0.25,Message="actual failure"}}};TestRunnerApi.Last.callbacks.RunFinished(result);
        var final=Call(new UnityTestBridge.Request{op="snapshot",owner=run.owner,job=run.job});Check(final.state=="finished"&&final.cases[0].message=="actual failure","framework final result missing");
        TestRunnerApi.Last.callbacks.RunStarted(root);var competing=Call(new UnityTestBridge.Request{op="discover",mode="PlayMode",job=new string('d',32)});Check(!competing.ok,"native foreign run did not fence discovery");TestRunnerApi.Last.callbacks.RunFinished(result);
        var instance=UnityEngine.Resources.FindObjectsOfTypeAll<UnityTestRunnerProvider>().Single();typeof(UnityTestRunnerProvider).GetMethod("Initialize",System.Reflection.BindingFlags.NonPublic|System.Reflection.BindingFlags.Instance).Invoke(instance,null);
        var resumed=Call(new UnityTestBridge.Request{op="observe"});Check(resumed.owner==run.owner&&resumed.job==run.job,"serialized owner/job rebound to a replacement");var stale=Call(new UnityTestBridge.Request{op="page",owner=run.owner,job=run.job,snapshot=final.snapshot,offset=8});Check(!stale.ok,"expired pre-rebind snapshot accepted");
        TestRunnerApi.DeferDiscovery=true;
        string interrupted=new string('e',32);
        var pending=Call(new UnityTestBridge.Request{op="discover",mode="EditMode",job=interrupted});
        Check(pending.ok&&pending.state=="discovering","discovery callback was not deferred");
        CompilationPipeline.Begin();
        var oldCallback=TestRunnerApi.DeferredDiscovery.Dequeue();
        oldCallback(root);
        var invalidated=Call(new UnityTestBridge.Request{op="snapshot",owner=pending.owner,job=interrupted});
        Check(invalidated.ok&&invalidated.state=="cancelled"&&String.IsNullOrEmpty(invalidated.catalog)&&invalidated.total==0,
              "pre-compilation discovery callback published a stale catalog");
        string freshJob=new string('f',32);
        var fresh=Call(new UnityTestBridge.Request{op="discover",mode="EditMode",job=freshJob});
        Check(fresh.ok&&fresh.state=="discovering","compilation prevented a fresh discovery");
        TestRunnerApi.DeferredDiscovery.Dequeue()(root);
        var freshCatalog=Call(new UnityTestBridge.Request{op="observe"});
        Check(freshCatalog.ok&&freshCatalog.job==freshJob&&freshCatalog.catalog==freshJob&&freshCatalog.state=="discovered",
              "fresh post-compilation discovery did not publish");
        Console.WriteLine("Unity producer logic: exact selection, callback data, paging, cancellation, competing-run fence and observer reinitialization checked");
    }
}
