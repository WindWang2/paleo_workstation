# Agent H
Baseline: 42296fd3
Scope: src/ui, src/app
Read in full: paleomainwindow close/save gate, coordinate pick, section setup, import queue runner, well-composite edit/export, facies schema wire, seismic pick autosave, LLM diagnostics export, main.cpp startup. Pattern-scanned the rest of src/ui and src/app (~600 files) for WA_DeleteOnClose, deleteLater tools, QSaveFile/write success toasts, “未就绪”/setEnabled(false), unsaved close, import retry timers, installEventFilter snapshots, and context-less connects.

## Findings

### H-01
- Sev: P1
- Conf: VERIFIED
- Loc: src/ui/pages/mappingworkbenchpage_schema.cpp:202
- Title: Project switch drops unsaved facies-classification drafts with no prompt
- Bug: Microfacies rows edited on the mapping workbench stay only in `m_faciesDrafts`. Opening another project (or any catalog rebind) clears that map and `m_faciesDirty`. The close/switch gate never asks, and saving the .qgz does not write these rows, so the classification the page told the user to save is gone.
- Evidence: `cellChanged` sets `m_faciesDirty` (schema.cpp:133-136). `catalogBound` always does `m_faciesDrafts.clear(); m_faciesDirty = false` then reloads from the new catalog (schema.cpp:202-204). `MappingWorkbench::bindCatalog` emits `catalogBound` (workflow/mappingworkbench.cpp:303). `AppContext` calls `bindCatalog` on project open (src/app/appcontext.cpp:853). `PaleoMainWindow::maybeSaveProject` (paleomainwindow.cpp:886-937) checks cluster drafts, stratigraphy, vector buffers, and `QgsProject::isDirty` only — not `m_faciesDirty`. A clean .qgz therefore returns true and the next bind wipes the draft.
- Not excluded because: concrete silent loss of a geological classification on the normal open/switch path, not a missing feature or a PaleoMainWindow size nit.
- Trigger: On the workbench schema table, change a microfacies name or code (status says the draft is unsaved). File → Open another project while the .qgz is clean. No save/discard dialog; the new project shows the stored classification.

### H-02
- Sev: P1
- Conf: VERIFIED
- Loc: src/ui/datapreview/datapreviewtabs.cpp:167
- Title: Closing a well preview or switching projects drops unsaved TOP edits
- Bug: Dragging a marker in “TOPs 编辑” dirties `EditSession` until “保存派生”. Closing that preview tab, or switching/closing the project, destroys the panel and keeps only the track-layout sidecar. The moved tops are not written and nobody is asked.
- Evidence: `m_tabs->setTabsClosable(true)` and `tabCloseRequested` calls `closeAssetTab`, which `deleteLater`s the page with no dirty check (datapreviewtabs.cpp:154, 167-170, 480-481). `resetProjectScopedState` calls `closeAllTabs` on `projectAboutToClose` (paleomainwindow.cpp:170-171, 734-735). `~WellCompositePanel` only calls `saveSessionState` (wellcompositepanel.cpp:47-49), which writes track specs (paneltracks.cpp:508-512), not the edit session. `saveDerived` is the only path that emits `derivedDocumentReady` and `markSaved` (paneledit.cpp:55-73), and it runs only from the “保存派生” button (panelsetup.cpp:250-252). `maybeSaveProject` does not read `EditSession::isDirty`.
- Not excluded because: same close/switch gate already prompts for vector edits, cluster drafts, and stratigraphy; this sibling editor is omitted, so the gate returns true and then deletes the edits.
- Trigger: Open a comprehensive well (or LAS) preview, 更多 → TOPs 编辑, drag a marker, then click the preview tab’s close button or open another project without clicking 保存派生.

### H-03
- Sev: P2
- Conf: VERIFIED
- Loc: src/ui/wellsection/wellsectionpanel.cpp:945
- Title: Well-section SVG/PDF export reports success when the device write fails
- Bug: The export button shows a failure dialog only when `exportTo` returns false. SVG ignores `QPainter::end()`, and PDF returns true before the painter’s destructor ends the device, so a full-disk or unwritable SVG/PDF leaves no error. The user keeps the path they just chose and sees no failure.
- Evidence: Button at wellsectionpanel.cpp:384-389: `if (!path.isEmpty() && !exportTo(path)) PaleoNotify::critical(...)`. SVG paints then `p.end(); return true;` (939-946). PDF paints then `return true;` with no `end()` check (958-965). The composite exporter was fixed to treat `!p.isActive()` and `!p.end()` as failure (wellcomposite/exportengine.cpp:441-456 and 536-538). CSV/PNG on this panel do check the write.
- Not excluded because: reachable export slot; failure is swallowed so the UI does not report it, which is the success-vs-write bug for this control.
- Trigger: 连井剖面 → 导出剖面图, choose SVG or PDF on a full volume or a read-only folder. The dialog closes with no “导出失败”.

### H-04
- Sev: P2
- Conf: VERIFIED
- Loc: src/ui/seismicsection/seismicsectiondock_internal.h:31
- Title: Seismic pick autosave ignores a failed commit, then project switch drops the picks
- Bug: Every pick undo command autosaves the session and discards the bool. If `QSaveFile::commit` fails, the picks stay on the section as if they were stored. `projectAboutToClose` then clears the in-memory session. Reopening loads the previous `.seispicks.json`, so the new picks are gone and no warning was shown.
- Evidence: `PickCommandBase::refresh` calls `m_dock->saveInterpretationSession()` and ignores the result (seismicsectiondock_internal.h:30-31). `AddPicksCommand::redo` calls `refresh()` (same header, redo at line 75). `saveInterpretationSession` returns `SeismicTaskService::saveSession` (seismicsectiondock_interpret.cpp:309-317). `saveSession` returns false when `write` is short or `commit` fails (services/seismictaskservice_interp.cpp:1177-1188). `resetProjectScopedState` calls `resetInterpretationState`, which replaces `m_session` with an empty session (paleomainwindow.cpp:765-766, seismicsectiondock_interpret.cpp:56). The manual “会话已保存” button does check the bool (seismicpickpanel.cpp:312-316); autosave does not.
- Not excluded because: a failed persisted write of interpretation picks, not an unfinished document-model item.
- Trigger: Pick a horizon on a section whose `.seispicks.json` cannot be committed (disk full, or the SEG-Y directory is not writable). Then switch projects and open the same volume again. The new picks are absent.

### H-05
- Sev: P2
- Conf: VERIFIED
- Loc: src/ui/paleomainwindow_sections.cpp:299
- Title: Replacing the section-draw tool leaves the setup dialog hidden
- Bug: “地图绘制任意折线” hides the setup dialog and switches to the constraint page. The dialog is shown again only on `captureCancelled` or `sectionExtractedFromMap`. Deactivating the tool by choosing another map tool does not emit `captureCancelled`, so the dialog stays hidden and `drawingFromSetup` stays true. Esc still restores it; picking Pan, identify, or a coordinate tool does not.
- Evidence: draw slot sets `*drawingFromSetup = true`, `showPage("constraint")`, `setup->hide()` (paleomainwindow_sections.cpp:299-301). Restore is `sectionCaptureCancelled` → `setup->show()` (243-250) and `sectionExtractedFromMap` when the flag is set (231-237). `SeismicSectionTool::deactivate` clears the rubber band and does not emit `captureCancelled` (qgis/seismicsectiontool.cpp:86-93). `captureCancelled` is emitted only from Escape (seismicsectiontool.cpp:227-232). `setMapTool` deactivates the previous tool.
- Not excluded because: the hidden dialog is the section-setup task the user just started; another map tool is a normal way to leave the draw.
- Trigger: Open 连井 / 时深对齐, click 地图绘制任意折线, then click Pan or the coordinate tool without pressing Esc. The setup dialog does not come back.

### H-06
- Sev: P2
- Conf: HIGH
- Loc: src/ui/ai/llmdiagnosticspanel.cpp:182
- Title: LLM diagnostics export says 已导出 on a short write
- Bug: The notice says the summary was exported whenever `QFile::write` does not return -1. A short write (disk full mid-file) still shows “已导出：&lt;path&gt;” and leaves a truncated text file.
- Evidence: `if (file.write(m_lastSummary.toUtf8()) < 0) { showNotice(失败); return; } showNotice(tr("已导出：%1").arg(path));` (llmdiagnosticspanel.cpp:182-186). Open failure is checked; byte count is not. Sibling writers require `write(bytes) != bytes.size()` (for example wellcomposite/paneltracks.cpp:292 and sequenceframeworkpanel.cpp:483).
- Not excluded because: the slot tells the user the export succeeded when the write did not finish.
- Trigger: Export a diagnostics summary to a volume that accepts the open but cannot take the full buffer.

## Covered and clean
- src/app/main.cpp: `--check-runtime` returns 0/1 and does not continue into a project write; `!ctx.ready()` returns 1 before `openPath`.
- src/ui/paleomainwindow_coordinates.cpp and src/ui/datapreview/previewmapcoordinates.cpp: `WA_DeleteOnClose` deletes the inner dialog and the wrapper is `deleteLater`’d; replacing an active pick tool emits `cancelled` before `deleteLater`.
- src/ui/pages/dataopsimportui.h `runPending` and src/ui/pages/dataops/dataopsimportqueue.h `finish`: manual retry and async `RetryWait` both call `runPending`, which restarts the timer when `hasPending()`.
- src/ui/paleomainwindow.cpp `maybeSaveProject` / `resolveDirtyLayerEdits`: vector buffers, cluster drafts, and stratigraphy are prompted before close or `openPath`.
- src/ui/wellcomposite/exportengine.cpp and paneltracks.cpp CSV: commit / `p.end()` failures are reported.
- src/ui/pages/mappingreferencepanel.cpp: wheel filter is reinstalled on `ChildAdded` and on `viewCreated` (canvas viewport). Not filed; a missed nested wheel here is a horizontal nudge, not a stuck edit.

## Not filed
- Import-queue “timer never restarts after empty”: the comment in `runPending` matches the current `else if (!m_timer->isActive()) m_timer->start()` plus `finish()` → `runPending()`.
- Agent dock / export-map actions start disabled and are enabled when the assistant attaches or the file menu is about to show.
- Section setup “正在加载” after a finished volume load: success is applied by `sectionVolumeChanged`, and `alignmentsChanged` calls `ensureSectionSetupVolume` again.
- `SequenceFrameworkPanel` dirty state: no production constructor under src/ui or src/app.
- Virtual asset view missing the Delete filter: the 删除… button still opens the soft-delete confirm.
- Well-readiness “正在检查” stuck: `mappingworkbench_predict.cpp` emits `wellReadinessReady` on abort.
