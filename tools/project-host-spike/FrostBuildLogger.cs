// SPDX-License-Identifier: Apache-2.0
// SLN-10: loaded by MSBuild from the shipped ProjectHost assembly. The same event fields
// reach live diagnostics and binary-log replay; never parse localized console sentences.
using System;
using System.Web.Script.Serialization;
using Microsoft.Build.Framework;
using Microsoft.Build.Logging;

public sealed class FrostBuildLogger : ILogger
{
    public LoggerVerbosity Verbosity { get; set; }
    public string Parameters { get; set; }
    readonly object gate = new object();
    public void Initialize(IEventSource source)
    {
        source.ErrorRaised += (_, e) => Write("error", e.Code, e.Message, e.File, e.LineNumber, e.ColumnNumber, e.ProjectFile);
        source.WarningRaised += (_, e) => Write("warning", e.Code, e.Message, e.File, e.LineNumber, e.ColumnNumber, e.ProjectFile);
    }
    void Write(string severity, string code, string message, string file, int line, int column, string project)
    {
        lock (gate)
        {
            string record = new JavaScriptSerializer().Serialize(new {
                Severity = severity, Code = code ?? "", Message = message ?? "", File = file ?? "",
                Line = line, Column = column, Project = project ?? ""
            });
            // MSBuild's console code page may be legacy; ASCII JSON keeps every Unicode scalar.
            var ascii = new System.Text.StringBuilder();
            foreach (char c in record)
                if (c >= 127) ascii.Append("\\u" + ((int)c).ToString("x4")); else ascii.Append(c);
            Console.WriteLine("FROSTBUILD1 " + ascii);
        }
    }
    public void Shutdown() { }

    // Replay never evaluates a project or invokes targets; it consumes an existing binlog.
    internal static void Replay(string path)
    {
        var replay = new BinaryLogReplayEventSource();
        var logger = new FrostBuildLogger();
        logger.Initialize(replay);
        replay.Replay(path);
        logger.Shutdown();
    }
}
