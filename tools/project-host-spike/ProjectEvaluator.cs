// SPDX-License-Identifier: Apache-2.0
//
// ProjectHost evaluation (SLN-2, SLN-3): the "Hello" and "Evaluate" requests of the JSON-lines
// protocol that frost::project::ProjectHostClient speaks (src/project/include/frost/project/project_host.hpp
// documents the whole protocol). Evaluate loads one project with the Microsoft.Build of the
// Visual Studio instance VsProbe registered and returns its evaluated items, imports and a few
// properties. It never runs a target and never writes a file.
/*
 * Locator. The host is the .NET Framework build (net472) because the VC targets and tasks need
 * .NET Framework MSBuild. VsProbe.Main registers the instance the way MSBuildLocator's
 * RegisterMSBuildPath does for a Visual Studio instance (microsoft/MSBuildLocator, MIT): an
 * AssemblyResolve handler binds Microsoft.Build* to the instance's MSBuild\Current\Bin\amd64, and
 * MSBUILD_EXE_PATH, VSINSTALLDIR and VisualStudioVersion name that instance, so the toolset (and
 * $(VCTargetsPath)) resolve exactly as in MSBuild.exe started from a developer prompt. FrostIDE
 * chooses the instance with vswhere (frost::build::findMsBuild) and passes its MSBuild directory.
 *
 * Evaluate. Global properties are exactly the request's "Property:<Name>" entries (the caller
 * sends Configuration, Platform and the Solution* properties). A fresh ProjectCollection per
 * request, unloaded afterwards, so an edited project or import is always read again. Items are
 * those of the requested types, in evaluation order: type, evaluated include, FullPath, the
 * project file that defines the item (the project itself or an import) and the requested
 * metadata values. Imports are the evaluation's imports in order, each path once.
 */
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using Microsoft.Build.Evaluation;

static class ProjectEvaluator
{
    // Protocol version; frost::project::kProjectHostProtocol must equal it.
    public const int Protocol = 1;

    public static object Hello(string msbuildDirectory)
    {
        string assembly = typeof(Project).Assembly.Location;
        return new {
            Op = "Hello", Protocol, Error = (string)null,
            MSBuildVersion = FileVersionInfo.GetVersionInfo(assembly).FileVersion,
            MSBuildAssembly = assembly, MSBuildDirectory = msbuildDirectory,
            VisualStudioVersion = Environment.GetEnvironmentVariable("VisualStudioVersion"),
            ProcessId = Process.GetCurrentProcess().Id
        };
    }

    public static object Evaluate(Dictionary<string, string> request)
    {
        string trusted;
        if (!request.TryGetValue("Trusted", out trusted) || trusted != "true")
            throw new InvalidOperationException("Trusted root required before evaluation");
        string path = Required(request, "Project");
        if (!Path.IsPathRooted(path)) throw new ArgumentException("Project path must be absolute");
        var globals = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var entry in request)
            if (entry.Key.StartsWith("Property:", StringComparison.Ordinal))
                globals[entry.Key.Substring("Property:".Length)] = entry.Value ?? "";
        if (!globals.ContainsKey("Configuration") || !globals.ContainsKey("Platform"))
            throw new ArgumentException("An evaluation needs the Configuration and Platform properties");
        string[] types = Split(Required(request, "ItemTypes"));
        string metadataList;
        string[] metadata = request.TryGetValue("Metadata", out metadataList) ? Split(metadataList) : new string[0];
        string propertyList;
        string[] properties = request.TryGetValue("Properties", out propertyList) ? Split(propertyList) : new string[0];
        string id;
        request.TryGetValue("Id", out id);
        using (var collection = new ProjectCollection(globals))
        {
            try
            {
                var project = collection.LoadProject(path);
                var items = new List<object>();
                foreach (string type in types)
                    foreach (var item in project.GetItems(type))
                    {
                        var values = new Dictionary<string, string>();
                        foreach (string name in metadata)
                            values[name] = item.GetMetadataValue(name);
                        items.Add(new {
                            Type = item.ItemType, Include = item.EvaluatedInclude,
                            FullPath = item.GetMetadataValue("FullPath"),
                            DefiningProject = item.Xml.ContainingProject.FullPath, Metadata = values
                        });
                    }
                var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                var imports = project.Imports.Select(i => i.ImportedProject.FullPath).Where(seen.Add).ToArray();
                var values2 = new Dictionary<string, string>();
                foreach (string name in properties)
                    values2[name] = project.GetPropertyValue(name);
                var configurations = project.GetItems("ProjectConfiguration").Select(i => i.EvaluatedInclude).ToArray();
                return new {
                    Op = "Evaluate", Id = id, Error = (string)null, Project = project.FullPath,
                    Configuration = globals["Configuration"], Platform = globals["Platform"], Items = items,
                    Imports = imports, Properties = values2, ProjectConfigurations = configurations
                };
            }
            finally
            {
                collection.UnloadAllProjects();
            }
        }
    }

    static string Required(Dictionary<string, string> request, string name)
    {
        string value;
        if (!request.TryGetValue(name, out value) || String.IsNullOrEmpty(value))
            throw new ArgumentException("The request has no " + name);
        return value;
    }

    static string[] Split(string list)
    {
        return list.Split(';').Select(s => s.Trim()).Where(s => s.Length != 0).ToArray();
    }
}
