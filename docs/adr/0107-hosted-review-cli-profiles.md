# ADR 0107: Explicit hosted review CLI profiles

- Status: implementation decision, 2026-11-20; runtime provider acceptance deferred.
- Scope: R16 UX-14 hosted review tool window. GitHub CLI (`gh`) and GitLab CLI (`glab`) are separately installed user tools, not bundled libraries. Their executables and licences remain owned by the user; no dependency or credential is redistributed.

The user chooses an absolute executable path, host and repository. Opening the tool window starts no process or network request. Account identity comes from the provider user API. Listing, metadata and review submissions use documented CLI REST commands; diff and checkout use documented provider commands. Responses are displayed only after successful actual process output and strict review-row validation. Unsupported GitLab request-changes is explicitly refused rather than mapped to a comment.

Requests use IProcessLauncher, owned process containment, cancellation, an execution deadline and explicit project/trust admission. Project replacement invalidates queued completion, cancels the request and cannot admit a new operation under the old context. The UI confirms checkout and each outgoing review body against host, repository, review number and captured commit. Review JSON travels on stdin. GitHub reviews carry `commit_id`; GitLab approvals carry `sha`. A diff is discarded unless the provider reports the captured head both before and after fetching it. Checkout revalidates the provider head before changing files, then requires an explicitly selected absolute Git executable to verify the actual local HEAD equals the captured commit. A race or verification failure reports that local state may have changed; no rollback or unchanged-worktree claim is made. CLI credential stores are never read by FrostIDE, and inherited provider/HTTP credential-debug modes are removed.

The first profile covers 100-row open-review pages, actual authenticated account identity, metadata, provider patches, checkout, comments, approval and GitHub request-changes. Provider JSON and displayed response sizes are capped after process collection; the shared process collector still owns its existing buffering policy. The follow-up source adds anchored line discussions and existing thread read/refresh/reply. Repository creation/sharing, account sign-in UI, multiline/file-comment creation, draft review batches and resolve/reopen controls are not claimed. Runtime provider conformance remains deferred.

Source contracts consulted: [GitHub CLI API](https://cli.github.com/manual/gh_api), [GitHub pull requests](https://cli.github.com/manual/gh_pr), [GitLab CLI API](https://docs.gitlab.com/cli/api/), [GitLab merge request diff](https://docs.gitlab.com/cli/mr/diff/), [GitLab approval SHA](https://docs.gitlab.com/cli/mr/approve/). These are public documented formats; no proprietary or GPL IDE source was consulted.

Authored tests cover both provider row formats, rejection of incomplete/duplicate identity rows, exact JSON stdin body/commit pinning, invalid target refusal, admission/cancellation before launch, genuine CLI-error propagation, changed-head diff rejection and zero-launch UI creation. No tool, provider, feature, test or WSL execution occurred during implementation.

The anchored profile reads actual paged file-patch APIs, not local-buffer line guesses. GitHub records
head/base identities; GitLab retains base/start/head diff refs. Metadata must agree before and after
loading a file page. Strict hunk counters derive old/new line coordinates, with actual original/new
paths retained for renames. GitHub change totals additionally reject truncated patch prefixes, even
when truncation occurs at a complete hunk. Missing/binary/oversized/incomplete patches remain explicit
unavailable rows. At most 100 files and 20,000 displayed patch lines form one immutable page.

A line POST rechecks provider refs immediately before launch and carries the reviewed commit/position.
A race after preflight therefore stays tied to the originally reviewed version; it is never silently
retargeted to a new head. Comment success requires a genuine returned ID, body and exact original
position/commit or thread identity. Unconfirmed outcomes preserve the draft and require comment refresh
before another send; FrostIDE never automatically retries a POST. Existing comments retain actual
original/current positions, original commit identities, resolution fields when supplied and provider
thread roots. Confirmed sends refresh actual comments; GitHub's page is newest-first. GitLab discussion
pages retain provider ordering. Replies target an existing provider discussion, including outdated
positions. Selection/anchor/project/trust changes revoke old work, and production trust revisions are
captured before confirmation and checked by workers/publication; credentials remain in the external CLI.

Additional source fixtures cover old/new/renamed lines, GitLab full diff refs, count/hunk truncation,
forged-row refusal, changed-base/head admission, actual response validation, original-position/thread
refresh, normal panel comment/reply flows and modal selection/trust revocation. They are authored for
later execution, not claims of provider acceptance. Protocol sources also include
[GitHub review comments](https://docs.github.com/en/rest/pulls/comments),
[GitHub file patches](https://docs.github.com/en/rest/pulls/pulls), and
[GitLab discussions](https://docs.gitlab.com/api/discussions/).

A shared atomic dispatch phase cell is independent of widget selection epochs. A possible outgoing POST
therefore requires an actual comment refresh before another send even if its response is discarded due
to a changed selected row, project or trust state. Oversized/malformed responses retain that uncertainty;
a successful CLI exit alone is not a confirmed comment. Deferred fixtures cover post-start selection
cancellation and oversized-response outcomes as well as ordinary confirmed submission/refetch/reply.
