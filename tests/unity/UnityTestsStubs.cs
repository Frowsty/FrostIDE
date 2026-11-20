// SPDX-License-Identifier: Apache-2.0
// Explicit public-API doubles for producer state-machine tests; never a Unity acceptance oracle.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
namespace UnityEngine {
    public class Object { public HideFlags hideFlags; }
    public enum HideFlags { HideAndDontSave }
    public class ScriptableObject:Object { public static T CreateInstance<T>() where T:ScriptableObject {var value=(T)Activator.CreateInstance(typeof(T),true);Resources.Values.Add(value);return value;} }
    public static class Resources { internal static readonly List<Object> Values=new List<Object>();public static T[] FindObjectsOfTypeAll<T>() where T:Object{return Values.OfType<T>().ToArray();} }
    public static class Application {public static string dataPath;}
    public static class Debug {public static void LogError(object message){throw new Exception("Producer startup: "+message);} }
    [AttributeUsage(AttributeTargets.Field)]public sealed class SerializeField:Attribute{}
    public static class JsonUtility {private static readonly JsonSerializerOptions options=new JsonSerializerOptions{IncludeFields=true};public static string ToJson(object value){return JsonSerializer.Serialize(value,value.GetType(),options);}public static T FromJson<T>(string value){return JsonSerializer.Deserialize<T>(value,options);} }
}
namespace UnityEditor {
    [AttributeUsage(AttributeTargets.Class)]public sealed class InitializeOnLoadAttribute:Attribute{}
    public static class EditorApplication {public static bool isCompiling,isPlayingOrWillChangePlaymode;}
}
namespace UnityEditor.Compilation {public static class CompilationPipeline {public static event Action<object> compilationStarted;public static void Begin(){compilationStarted?.Invoke(null);} } }
namespace FrostIDE.UnityIntegration {internal static class UnityBrokerClient {internal static string PhysicalProject(string root){return System.IO.Path.GetFullPath(root);} } }
namespace UnityEditor.TestTools.TestRunner.Api {
    public enum TestMode {EditMode,PlayMode}
    public enum TestStatus {Passed,Failed,Skipped,Inconclusive}
    public interface ITestAdaptor {bool IsSuite{get;}IEnumerable<ITestAdaptor> Children{get;}string UniqueName{get;}string Name{get;}string FullName{get;} }
    public interface ITestResultAdaptor {ITestAdaptor Test{get;}IEnumerable<ITestResultAdaptor> Children{get;}double Duration{get;}string Message{get;}string StackTrace{get;}TestStatus TestStatus{get;} }
    public interface ICallbacks {void RunStarted(ITestAdaptor test);void RunFinished(ITestResultAdaptor result);void TestStarted(ITestAdaptor test);void TestFinished(ITestResultAdaptor result);}
    public sealed class Filter {public TestMode testMode;public string[] testNames;}
    public sealed class ExecutionSettings {public Filter filter;public ExecutionSettings(Filter value){filter=value;} }
    public sealed class TestRunnerApi:UnityEngine.ScriptableObject {
        public static TestRunnerApi Last;public static ITestAdaptor Tree;public ICallbacks callbacks;public ExecutionSettings executed;public string execution;public bool cancelAccepted;
        public void RegisterCallbacks<T>(T value)where T:ICallbacks{callbacks=value;Last=this;}public void UnregisterCallbacks<T>(T value)where T:ICallbacks{if(Object.ReferenceEquals(callbacks,value))callbacks=null;}
        public void RetrieveTestList(TestMode mode,Action<ITestAdaptor> callback){callback(Tree);}
        public string Execute(ExecutionSettings settings){executed=settings;execution=Guid.NewGuid().ToString();return execution;}
        public static bool CancelTestRun(string id){if(Last==null||Last.execution!=id)return false;Last.cancelAccepted=true;return true;}
    }
    public sealed class FakeTest:ITestAdaptor {public bool IsSuite{get;set;}public IEnumerable<ITestAdaptor> Children{get;set;}=new ITestAdaptor[0];public string UniqueName{get;set;}public string Name{get;set;}public string FullName{get;set;} }
    public sealed class FakeResult:ITestResultAdaptor {public ITestAdaptor Test{get;set;}public IEnumerable<ITestResultAdaptor> Children{get;set;}=new ITestResultAdaptor[0];public double Duration{get;set;}public string Message{get;set;}="";public string StackTrace{get;set;}="";public TestStatus TestStatus{get;set;} }
}
