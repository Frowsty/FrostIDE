# ADR 0071: Unity editor controls (work in progress)

Status: accepted bounded controls checkpoint; remaining Unity workflows and limitations below stay open.

The Unity editor-only package announces an ephemeral loopback endpoint and per-domain secret over
FrostIDE's existing project-keyed local IPC broker. The broker authenticates the OS user with a protected
Windows named pipe or Linux peer credentials. The Unity client checks the broker's OS user before sending
credentials. This is an OS-user boundary, not proof that a same-user process is Unity; a reported PID is
only a label. The IDE validates the physical project and trust, and requires explicit selection when
several editors are present.

Each connection uses a fresh client challenge and server nonce. The server proves its secret in a
length-prefixed UTF-8 HMAC transcript bound to the project, domain session and both challenges. Only after
checking that proof does the IDE send a separately domain-tagged MAC over the exact project, session,
request ID, command, TTL and both challenges; the bearer secret is never sent over loopback. The server
consumes each connection once before queuing, rejecting captured requests on a different nonce without an
ever-growing replay cache. It bounds frame sizes, connections, queued work and lifetime. Commands are
executed on `EditorApplication.update` after liveness, cancellation and expiry checks. Replies report the
observed state. Cancellation cannot undo a command that already ran; an interrupted request is uncertain.

Unity's public `SessionState` holds a random per-editor instance ID and domain generation across assembly
reload. A new domain receives a new session and secret; an older generation cannot replace a newer live
one. A previously selected instance can reconnect and the IDE fetches its observed state without claiming
that an interrupted action succeeded. Different instances remain separately selectable. Entering play may
itself trigger reload before the original reply. A frame step is accepted only while playing and paused,
and must observe a higher Unity frame count.

The portable C# protocol test verifies transcript tampering, one-shot replay and over 4,096 fresh requests
on Windows and Linux .NET. Windows workbench tests exercise invalid server proof, trust and identity,
selected-instance reload, a delayed old reply and observed replacement state. A Windows .NET mock editor
loop exercises challenge, wrong-token and captured-replay checks and play/pause/step/resume/stop. These are
not Unity Editor acceptance. A later Windows Unity 6000.6.4f1 Editor probe used a synthetic broker and
control client: it observed play, pause, frame-advancing step, resume, stop, and a domain-session change.
The Editor exited normally. Its five newly observed GameView preference changes were subsequently restored
against the exact typed snapshots, and the complete key matched the pre-run snapshot. A later Windows run
used the production Workbench project broker and control panel in a test rig with the actual Unity Editor.
The independent editor observer confirmed stopped state, play with a new domain generation, pause, a step
from frame 107 to 108, resume, stop, and that trust revocation prevented another play. All seven Workbench
checkpoints passed, source and metadata were unchanged, the editor job exited normally, and the full
EditorPrefs key matched its pre-run snapshot. This is a Workbench test rig, not a staged application-window
test, and does not independently exercise a transient disconnect. Astra approved the production source and
combined integration. Combined Windows and native Linux functional gates passed. Linux Unity Editor
acceptance remains required.

The combined-main Editor run initially timed out waiting for Stop; its cause remains unconfirmed. A later
diagnostic run failed earlier while publishing a shared observer-request file. The scratch handshake now
uses independent immutable request markers, avoiding concurrent read/truncate access to that file; this
does not establish the cause of either prior failure. The test retains all state assertions and records
Stop status transitions and an independent observer snapshot on timeout. Two consecutive combined-main
runs then passed all seven checkpoints, with observed Stop acknowledgements after 53 ms and 81 ms. Both
owned Editor jobs exited normally, source/meta were unchanged, and nine preference changes per run were
restored to their exact current typed baselines. These outcomes do not erase the failed runs or recover the
earlier lost preference value below. Busy controls currently ignore another command while waiting;
explicit busy feedback and recovery after a same-domain transport interruption remain follow-up work.

The first isolated control probe changed a Unity-owned EditorPrefs value whose original value was not
durably saved. Its current typed value is retained under current-user DPAPI; no older value was substituted.
Further Unity launches require coordination and review of the external scratch harness. That harness writes
fsync'd encrypted typed snapshots, encloses its suspended child and descendants in a Windows job before
resume, withholds preference restoration unless the job is verified empty, and monitors for other known
Unity preference writers. Its process detection uses polling and cannot prove that a very short-lived
external writer did not run between samples. Concurrent same-value edits during a run also cannot be
attributed from registry snapshots alone; neither limitation is an acceptance claim.

Logs, Unity tests, debugging and the remaining UNITY-02/03/04 workflows remain open. No Unity or other IDE
implementation source was copied. Public Unity API: [EditorApplication](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/EditorApplication.html),
[isPlaying](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/EditorApplication-isPlaying.html),
and [SessionState](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/SessionState.html).
