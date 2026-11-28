# Agent D
Baseline: 42296fd3
Scope: src/services
Read in full: paleotaskservice.{h,cpp}, jobrunner.{h,cpp}, seismiclinecache.cpp, seismicclusterlabels.cpp, wellfaciesinputstore.cpp, wellfaciesinputinspection.cpp, seismicdataaccess.cpp, pythonenvironmentconfig_io.cpp. Substantial reads: evidenceindex.cpp (open/sync/readVersion/query), wellfaciesinputplan.cpp, seismichorizoncluster.cpp, seismichorizonscatter.cpp, seismictaskservice.cpp, seismictaskservice_data.cpp, seismictaskservice_slice.cpp (extract + section cache), seismictaskservice_attr.cpp (time origin + volume scan), seismictaskservice_transcode.cpp (paged LOD cleanup), previewdoc.cpp (section/LAS), authenticatedartifactreader.cpp, operationretention.cpp, areawellchart.cpp (curve window).
Pattern-scanned: layer busy mark/release, QtConcurrent/worker lambdas, finish(true)/empty-QString success, removeDatabase/busy_timeout, generation compare, QSql string build, short QFile::read treated as EOF.

## Findings

### D-01
- Sev: P2
- Conf: VERIFIED
- Loc: src/services/evidenceindex.cpp:271
- Title: GDAL open failure is stored as a finished index and never retried
- Bug: A vector product whose catalog sha is already known is recorded with an error and zero evidence rows. The next sync treats that sha as a complete index, so the version stays empty after the file becomes readable.
- Evidence: `readVersion` returns on `GDALOpenEx` failure (line 271) and on “no vector layer” (line 279) after setting `*versionError` only. `*truncated` stays false. The mid-scan GDAL failure path at line 411 sets `*truncated = true` specifically so line 655 will not skip. Line 655 skips whenever `sha` matches and the version is not in `truncatedExisting`, and it does not look at `indexed_versions.error`. `MappingWorkbench::computeIndexSync` (`src/workflow/mappingworkbench_impact.cpp:1391`) is the sync caller; a later successful sync with the same catalog sha hits the skip.
- Not excluded because: this is a stuck empty evidence result, not the unfinished document-model migration in TODOS.md.
- Trigger: Sync a managed `mapping_product` vector version while GDAL cannot open it (locked, not flushed, or not a vector yet). Sync again after the file is fine. Queries for that version stay empty and the job still completes.

### D-02
- Sev: P2
- Conf: VERIFIED
- Loc: src/services/evidenceindex.cpp:276
- Title: Missing source_suffix layer is indexed as layer 0 and marked complete
- Bug: Evidence rows come from the first OGR layer instead of the named product layer. The file sha is stored with no error and `truncated=0`, so later syncs do not correct it.
- Evidence: If `layerName` is non-empty and `GDALDatasetGetLayerByName` fails, line 278 substitutes `GDALDatasetGetLayer(ds, 0)` and continues. No `versionError` is set. `synchronize` then `writeVersionRows` with the catalog sha. Callers stamp suffixes such as `|layername=facies`, `|layername=samples`, `|layername=cluster_patches`, and `|layername=features`. `computeIndexSync` persists the result.
- Not excluded because: the stored rows are the wrong features, not a style or include-spread note.
- Trigger: Sync a multi-layer GPKG whose `source_suffix` names a layer that is not present (renamed or not the first layer). Evidence review shows layer 0 until the file sha changes.

### D-03
- Sev: P2
- Conf: VERIFIED
- Loc: src/services/evidenceindex.cpp:462
- Title: Failed Index::open leaks the SQLite connection and the next open does not remove it
- Bug: Each failed open leaves a process-wide QSQLITE connection holding `artifacts/evidence-index.sqlite`. The long-lived workbench index retries on every reverse query, so connections and file handles accumulate and `close()` only drops the latest name.
- Evidence: Line 462 assigns a new connection name and line 465 calls `addDatabase` before any failure return (`db.open` at 468, pragma failure, schema-too-new at 536). Those returns do not `removeDatabase`. `m_open` is set only at line 562, so the next `open` does not call `close()` (`if (m_open) close()` at 450). `~Index`/`close` (line 567) removes only the current `m_connection`. `MappingWorkbench::evidenceReverseQuery` (`src/workflow/mappingworkbench_impact.cpp:1489`) keeps one `Index` and calls `open` whenever `!isOpen()`.
- Not excluded because: this pins the index file and grows connections on a normal retry, not a naming nit. The UUID change fixed collisions, not this leak.
- Trigger: Open a project whose evidence index fails schema or pragma checks, then run evidence search again. Each attempt adds another live connection.

### D-04
- Sev: P2
- Conf: HIGH
- Loc: src/services/seismictaskservice_attr.cpp:49
- Title: Unreadable SEG-Y delay is stored as a 0 ms attribute time origin
- Bug: Time-slice products and attribute-volume headers get `timeMs` / `startTimeMs` of 0 when the trace-header read fails. A real non-zero delay is shifted to the start of the trace and then cached.
- Evidence: `readSgyStartTimeMs` returns `0.0` when open, the 3600-byte read, seek, or the 2-byte read fails (lines 49–65). `0.0` is also a legal delay, so the caller cannot tell. `startAttributeTimeSlice` sets `result->timeMs = t0Ms + sampleIndex * dtMs` (line 313) and the GeoTIFF grid copies `result->timeMs`. `startAttributeVolume` copies the same `t0Ms` into `info.startTimeMs` (line 851), which the SATV header persists. A later cache hit trusts that header.
- Not excluded because: this is a wrong persisted time on the attribute product, not a missing feature.
- Trigger: Run an attribute time slice or volume scan when the SEG-Y cannot be opened again or the header read is short (file busy, sharing violation). The scan still succeeds and publishes time 0.

### D-05
- Sev: P2
- Conf: HIGH
- Loc: src/services/seismictaskservice_transcode.cpp:628
- Title: Paged transcode reports success when an obsolete LOD file is not removed
- Bug: A rebuild whose plan no longer includes L1/L2/L3 can leave those files in place and still finish as success. The next paged open starts on the coarsest sibling it finds, which can be the previous volume’s decimated amplitudes.
- Evidence: After the new pyramid is accepted, lines 622–629 call `std::filesystem::remove` for every level not in `plannedLevels` and ignore both the return value and `ec`. Line 631 then `return QString()`, which `PaleoTask` treats as success when cancel is not set. `datasetEntryFor` opens paged data with `options.progressiveLod = true` (`seismictaskservice.cpp:80`), commented as sibling discovery from the coarsest level. The registry entry is dropped only in the finished slot, after this delete attempt, so a sibling still mapped by the previous dataset is a normal reason for `remove` to fail on Windows.
- Not excluded because: leftover coarse LOD is a wrong displayed section, and the success return gives no retry.
- Trigger: Transcode a volume that already has a pyramid, while that paged dataset is still open, into a plan that drops a level (`buildLod=false`, or a smaller size class). If delete fails, the task still succeeds and the next view can show the old coarse level.

### D-06
- Sev: P3
- Conf: HIGH
- Loc: src/services/wellfaciesinputstore.cpp:60
- Title: Cancelling a stored well-facies load is reported as a hash mismatch
- Bug: The caller is told the frozen input failed its manifest hash. A good on-disk input looks corrupt, so a cancelled load is not retried as a cancel.
- Evidence: `read` returns an empty `QByteArray` when `cancelled()` is already true (line 60) and also when the read itself fails. `loadInput` then uses one test for both: `bytes.isEmpty() || hash != manifestHash` returns `stored_input_hash_mismatch` (line 68). The same collapse applies to each segment body at line 91.
- Not excluded because: the tool-facing code is wrong on a cancel path the function already accepts.
- Trigger: Call `loadInput` with a cancel predicate that is true, or that becomes true before a body read. The result code is `stored_input_hash_mismatch`, not `cancelled`.

## Covered and clean

- `PaleoTaskService` same-layer busy: each task is `task-<id>`. `onTaskFinished` (paleotaskservice.cpp:449) always `releaseLayerBusy`s that id. `PaleoProjectStore::releaseLayerBusy` removes only that holder. The old “first N-1 never release” counter is gone. Shutdown that actually drains also releases leases the queued `finished` signal did not deliver (line 309).
- `JobRunner::start` refuses a second claim while `busy()`, writes commit errors back, and `failPublication` on a failed commit. It does not emit `jobCompleted(..., true)` after `commit` returns false.
- Evidence SQL: `predicateFor` is a fixed table; user text is bound, including `LIKE` with escapes. `LIMIT`/`OFFSET` are clamped integers. `busy_timeout` is set. Connection names are UUIDs (the leak above is the remaining hole).
- `seismiclinecache`: generation is compared before insert, and `clearSeismicLineCache` bumps it. The key also includes size, mtime, metadata change time, and a head/tail hash, and a changed identity during read is rejected. `clearSeismicLineCache` currently has no production caller; replacement is detected by the key, not by that bump.
- `SgyDataCache::Find` returns `shared_ptr`. Slice workers join before the buffer goes out of scope. `startDataSlice` / `startDataPlane` recompute the native key before `Store` and drop the image if the file identity changed.
- Well-facies `prepareInput` re-reads the model head and fails with `input_changed_during_read` when generation or sequence moves. `storeInput` does not return a reference unless the manifest `QSaveFile::commit` succeeds.
- Python environment `save` takes a lock and rejects a mismatched `expectedRevision` before commit.

## Not filed

- `startBounded` increments `active` before `PaleoTaskService::start`. A null task would leak the count, but `setSeismicTaskService` runs once at startup before tasks, and nothing else waits on `activeTaskCount`. No current user-visible caller.
- Attribute-volume cancel that lands after `paleoReplaceFile`: the finished callback says cancelled and omits the path, but the next identical scan cache-hits the completed SATV. Narrow race with a recovery.
- `seismiclinecache` calls `cancel()` while holding its mutex. Current callers only load an atomic flag, so this is not a deadlock yet.
- `readSgyStartTimeMs` / `estimateVolumeBytesFromSgy` short header reads. The volume-byte estimate only changes the LOD plan (commented as non-authoritative). The delay-0 case is D-04; the LOD plan change is not a wrong amplitude.
- Document-model operation recovery and cross-store commit: still the open TODOS.md migration. No new “success returned after a failed commit” in the services reads above.
