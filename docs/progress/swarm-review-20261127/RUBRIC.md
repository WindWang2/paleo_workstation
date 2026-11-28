# Swarm review rubric — 2026-11-27

Baseline: `42296fd3` (`master`, 2026-11-27, 整合地震聚类与井赋相、Qt 工作区和 Pi Agent).
Mode: read-only. Write only your own findings file. Do not edit source, tests, baselines, or git state.
Do not commit. The dirty worktree files below are someone else's work — leave them:

- `docs/seismic/CLUSTER_RESULTS_AND_WELL_ASSIGNMENT_PLAN.md`
- `prototype/Paleo-Workspace-Prototype.zip`
- `prototype/workspace-first/fonts/OFL-IBM.txt`
- `prototype/workspace-first/fonts/OFL-JetBrains-Mono.txt`

## What to file

A finding must change user-visible or data-visible behavior on a path that existing
callers can reach. Cite `path:line` you actually opened. If you cannot quote the
controlling condition, do not file it.

File these:

1. Silent data loss, truncated write, or a persisted result that is wrong.
2. Crash, use-after-free, deadlock, or a task/layer gate that stays locked.
3. A reachable bounds bypass (memory, row, or file-size budget).
4. Secret leak, path escape, or sandbox bypass.
5. A wrong geological number: depth/time shift, NaN punch-through, mixed units, off-by-one on a sample or horizon.
6. Failure swallowed so the UI or tool result says success.
7. A race that corrupts a catalog, project file, or in-flight operation record.

Severity:

- P1: data loss, wrong persisted geology, hang that blocks a workflow, or a trust-boundary break on a normal path.
- P2: wrong result or stuck state on a plausible path, with a clear recovery missing.
- P3: real defect with a narrow trigger or a leftover that the next caller will hit.
- P4: confirmed defect with no current production caller. Say the caller is empty.

Confidence: `VERIFIED` (you read the callee and a caller), `HIGH` (controlling condition is local and unambiguous), `SUSPECTED` (needs a runtime check). Prefer fewer `VERIFIED` items over a long `SUSPECTED` list.

## What not to file

- Style, naming, comments, DESIGN token nits, file-size or include-spread baseline items.
- Missing features and "should be faster" without a wrong result or a hang.
- Speculative null crashes with no caller that passes null.
- Layering include nits. The checker already gates those.
- Known unfinished work in `TODOS.md`:
  - document-model migration still open (operation recovery for unmigrated businesses, bounded migration of old projects, generic multi-system time-depth, facet consumers);
  - remaining Agent command migration, cross-store global commit, unsupported depth maps;
  - Python shell, network, online install, persistent interpreter, automatic generation GC — explicitly deferred;
  - desktop / high-DPI visual acceptance.
- God-class split of `PaleoMainWindow` and unifying the four undo stacks. Those are known design deferrals. File only a concrete wrong behavior inside them.
- i18n catalog gaps (Chinese source, missing `paleo_en.ts`).

## Output

Write `docs/progress/swarm-review-20261127/findings/agent-<ID>.md` in UTF-8 Markdown.

```
# Agent <ID>
Baseline: 42296fd3
Scope: <dirs>
Read in full: <count and the important paths>
Pattern-scanned: <what>

## Findings

### <ID>-01
- Sev: P2
- Conf: VERIFIED
- Loc: src/foo/bar.cpp:120
- Title: <one line>
- Bug: <what the user or the file ends up with>
- Evidence: <the condition, the missing cleanup, the caller>
- Not excluded because: <one line>
- Trigger: <how a normal call hits it>

## Covered and clean
<short list of hot files you read and found no defect in>

## Not filed
<candidates you rejected and why>
```

Cap the file at 12 findings. If you have more, keep the highest severity and list the rest as one-line overflow. Empty findings is a valid result — write the coverage section anyway.
