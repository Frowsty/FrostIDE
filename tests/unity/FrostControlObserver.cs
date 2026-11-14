// SPDX-License-Identifier: Apache-2.0
// Real Editor-only legacy fixture observer. Values come from Unity public APIs on its update thread.
using System;
using System.IO;
using System.Text;
using UnityEditor;
using UnityEngine;

[InitializeOnLoad]
internal static class FrostControlObserver
{
    [Serializable]
    private sealed class Observation
    {
        public string checkpoint;
        public bool playing, paused;
        public int frame, generation;
    }

    private static readonly string[] Checkpoints = { "initial", "playing", "paused",  "stepped",
                                                     "resumed", "stopped", "revoked", "stoptimeout" };
    private static readonly string Exchange = Environment.GetEnvironmentVariable("FROST_UNITY_EXCHANGE");
    private static readonly string Project = Environment.GetEnvironmentVariable("FROST_UNITY_FIXTURE_PROJECT");

    static FrostControlObserver()
    {
        if (!String.IsNullOrEmpty(Exchange) && !String.IsNullOrEmpty(Project))
            EditorApplication.update += Observe;
    }

    private static void Observe()
    {
        string actual = Directory.GetParent(Application.dataPath).FullName;
        if (!String.Equals(Path.GetFullPath(actual), Path.GetFullPath(Project), StringComparison.OrdinalIgnoreCase))
            return;
        try
        {
            foreach (string checkpoint in Checkpoints)
            {
                string destination = Path.Combine(Exchange, checkpoint);
                if (!File.Exists(Path.Combine(Exchange, "observe-request-" + checkpoint)) || File.Exists(destination))
                    continue;
                var observation =
                    new Observation { checkpoint = checkpoint, playing = EditorApplication.isPlaying,
                                      paused = EditorApplication.isPaused, frame = Time.frameCount,
                                      generation = SessionState.GetInt("FrostIDE.UnityControls.Generation", 0) };
                // Publish only a complete flushed observation, including across domain reload.
                string temporary = destination + ".tmp";
                byte[] bytes = new UTF8Encoding(false).GetBytes(JsonUtility.ToJson(observation));
                using (var file = new FileStream(temporary, FileMode.Create, FileAccess.Write, FileShare.None))
                {
                    file.Write(bytes, 0, bytes.Length);
                    file.Flush(true);
                }
                File.Move(temporary, destination);
            }
            if (File.Exists(Path.Combine(Exchange, "quit-request")))
            {
                EditorApplication.update -= Observe;
                EditorApplication.Exit(0);
            }
        }
        catch (IOException)
        {
            // A later real update retries an unavailable marker. The consumer's original deadline applies.
        }
    }
}
