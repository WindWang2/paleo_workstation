# Agent A
Baseline: 42296fd3
Scope: src/io src/metadata
Read in full: encodingdetect.cpp, atomicwrite.h, atomicfile.h, metastore.cpp, managedpath.cpp, mapsnapshot.cpp, paleoprojectfile.cpp (write path), wellfileparsers.cpp (stream sniff + device entry points), welltableimport.cpp, wellfileread.cpp, welllayerdepthimport.cpp, boundedread.h, dataimport_session.cpp storeManagedRaw, constraintstore.cpp readHistory/copyAudit, cachebudget.cpp enforce, lrucache.h evictLocked. Sampled lasparser depth window, dlis/lis/segy, rasterpyramid tile append, paleoprojectstore commit, projectcopy, zip/sfpkg writers, wellfactorfile, paleomapfiles, originalpackage.
Pattern-scanned: user_version, busy_timeout, QSaveFile commit, write() size, mkpath error returns, fromUtf8 / decodeText / U+FFFD, NaN and monotonic depth, fileSizeLimit, readAll, history page cursor, path join onto a project directory.

## Findings

### A-01
- Sev: P2
- Conf: HIGH
- Loc: src/io/wellfileparsers.cpp:494
- Title: Streaming well-table import decodes a trailing GBK character as U+FFFD
- Bug: A well name, top name, or well id that is legal GB18030 is stored with U+FFFD in place of the last character. Import still returns success, so later well-to-top joins miss that row.
- Evidence: `StrictUtf8Stream::strictAtEof()` returns `ok` even when `rem > 0` (line 494). `sniffStreamEncoding` then sets `TextEncoding::Utf8` (lines 574–576) and never tries GB18030. `decodeStreamLine` finishes with `QString::fromUtf8` (line 599), which substitutes U+FFFD. `textColumn` accepts any non-empty name (lines 165–168). `EncodingDetect::detect` already rejects this shape: an ASCII body plus a final GBK pair whose lead is in E0–EF (唰 = E0 A7, and the other 1023 leads called out at encodingdetect.cpp:138–145) is not strict UTF-8 after the incomplete tail is stripped, so the whole-file path decodes GB18030. The device path does not. `readImportWellInput` calls `parseWellHeadDevice` / `parseWellTopsDevice` / `parseTimeDepthDevice` / `parseDeviationDevice` (welltableimport.cpp:41–51) for ordinary text tables.
- Not excluded because: persisted well identity is wrong, not an encoding-style nit or an i18n catalog gap.
- Trigger: Import a text well head, tops, time-depth, or deviation file whose bytes are valid UTF-8 up to a final GBK character that looks like an unfinished UTF-8 sequence, with no non-continuation byte after it (no trailing newline). Example tail: `E0 A7`.

### A-02
- Sev: P2
- Conf: VERIFIED
- Loc: src/io/welltable.cpp:291
- Title: Depth resolution rewrites a tops table with the base-of-layer rows dropped
- Bug: The normalized `well_data.dat` keeps TVD→MD tops and silently loses every `# PaleoLayerInterval` base. Preview then prefers that managed dat (services/previewwellbases.cpp:23–28) and the original bases are not used.
- Evidence: `readWellTableBases` on a `.dat` does `if (!readBoundedFile(path, 64MB, &bytes)) return {}` with no error (welltable.cpp:287–291). For csv/tsv/txt it uses `readWellDelimitedTable`, which fails above 64MB (welltabletext.cpp:54–56), and the same function still returns an empty vector when `workbook.ok` is false (welltable.cpp:318–324). `resolveImportedWellDepths` loads tops through a separate 500MB hash-checked read (`contents`, welllayerdepthimport.cpp:15–18 and 92), converts any TVD-only row, and if `resolved > 0` calls `storeNormalizedWellTable` with those empty bases (lines 95–108). Folder import always runs that step after a clean plan (dataimport_folder.cpp:480). Import itself accepts the same file up to `LasParser::fileSizeLimit` (500MB, welltableimport.cpp:23).
- Not excluded because: this deletes persisted layer-base depths on a successful import, not an unfinished migration listed in TODOS.md.
- Trigger: Import a stratification table larger than 64MB (or any `.dat` whose bounded read fails) that has base depths and at least one top that is TVD without MD, with a usable deviation or time-depth model for that well.

### A-03
- Sev: P2
- Conf: HIGH
- Loc: src/io/dataimport_session.cpp:169
- Title: Managed raw copy reports success when the last flush fails
- Bug: The catalog row stores the full source SHA-256, but the file under `artifacts/` is the truncated tail of a buffered write. A later reader that trusts the catalog hash parses a short well or grid file as complete.
- Evidence: `storeManagedRaw` checks each `write()` (lines 154–159) then ignores `flush()` and `close()` (lines 169–170), `paleoReplaceFile`s the partial over the destination, and returns true with the hash of the bytes read from the source (lines 171–183). It never re-hashes the file it just published. `QFile` is buffered; a full `write()` only means the buffer accepted the chunk. The import caller treats that hash as the managed checksum and, when it matches the pre-copy source hash, keeps the version (dataimport_file.cpp:404–418).
- Not excluded because: the managed project bytes and the checksum that is supposed to describe them diverge.
- Trigger: Copy a managed raw (the non-external import path) when the disk fills, or the final flush fails, after `write()` has already returned the full chunk size.

### A-04
- Sev: P2
- Conf: HIGH
- Loc: src/io/constraintstore.cpp:580
- Title: Constraint audit copy treats a bad page as the end of history
- Bug: A published constraint snapshot is missing older audit events (including deletes) while `copyAudit` returns success. The map product is then recorded as if the history copy finished.
- Evidence: `readHistory` skips a row whose `event_json` is not an object and does not update `minFid` for it (constraintstore.cpp:155–161). It also stops the `GetNextFeature` loop on NULL without reading `CPLGetLastErrorMsg`, so an I/O error is indistinguishable from EOF (lines 147–163). `copyAudit` breaks when `page.isEmpty() || minFid == 0` (lines 577–581). A page of 200 corrupt events, or a short read that yields no accepted row, therefore ends the cursor even though older `fid`s were never visited. An empty collection returns true (line 583). The mapping workbench removes the gpkg only when `copyAudit` returns false, then records the snapshot (src/workflow/mappingworkbench_edit.cpp:802–819).
- Not excluded because: the snapshot’s audit chain is persisted short, which is a wrong project result rather than a missing feature.
- Trigger: Publish or snapshot a horizon whose `constraint_audit` page (200 rows, newest first) is entirely corrupt JSON, or whose GDAL read fails before any valid row of that page is returned.

### A-05
- Sev: P2
- Conf: VERIFIED
- Loc: src/io/wellfileread.cpp:9
- Title: Preview and project reads of well tables ignore the import size budget
- Bug: `readWellHeads`, `readWellTops`, and `readTimeDepth` pull the whole file into memory. A multi-gigabyte text table that import would reject OOMs the process on preview or on a framework pass over the catalog.
- Evidence: `bytes()` is `out = file.readAll()` with no `LasParser::fileSizeLimit` check and no `readBoundedFile` (wellfileread.cpp:6–11). The import path refuses the same paths above that limit before parsing (welltableimport.cpp:23). Callers that hit the uncapped read: `PreviewDocService::wellHeadsAt` / `wellTopsAt` / `timeDepthAt` (src/services/previewdoc.cpp:368, 378, 387), `FrameworkService` tops collection (src/services/frameworkservice.cpp:83), and `readTimeDepth` from project data / well readiness (src/services/projectdata.cpp:329, src/services/wellreadiness.cpp:159).
- Not excluded because: this is a reachable file-size budget bypass, not a request to make the parser faster.
- Trigger: Open or diagnose a well-head, tops, or time-depth text file larger than 500MB (the import guard) via preview or the framework tops scan.

### A-06
- Sev: P3
- Conf: HIGH
- Loc: src/io/boundedread.h:49
- Title: Bounded whole-file read accepts a short read when a limit is set
- Bug: Callers that asked for the whole file under a cap receive a truncated buffer and a true return. Delimited well text, workbook XML, and composite XML then parse that prefix as the complete document.
- Evidence: The header comment says a short read returns false. The check `bytes.size() != size` runs only for `limit < 0` (boundedread.h:49–57). Every production caller passes a positive limit: welltabletext.cpp:54 (64MB), welltable.cpp:291 (64MB), wellcompositexml.cpp:985 and welllayerdepthimport.cpp:17 (500MB), outsourceworkbook.cpp:1118 (512MB). `f.error()` is checked, so a short `QFile::read` that does not set an error (file shrunk, or a filesystem that reports `size()` then returns EOF) is success. `readWellDelimitedTable` then parses `raw` with `ok` set from the header alone (welltabletext.cpp:54–58).
- Not excluded because: the function’s own contract is a failed read, and the truncated bytes become the table the importer keeps.
- Trigger: A well text, `.dat` base read, composite XML, or outsource workbook whose `size()` is within the cap but `read()` returns fewer bytes without setting `QFile::error`.

## Covered and clean
- `MetaStore::ensureUserVersion` (metastore.cpp:177–208): `user_version` is written after the migration steps and before `COMMIT`, inside `BEGIN IMMEDIATE`. `busy_timeout` is set on first open (line 87). Not the old bump-then-DDL order.
- `paleoWriteFileAtomic` / `writeFileAtomic` / `writeProjectFile`: size-checked write, `commit` checked, direct-write fallback off.
- `LasParser::parseDepthRange` monotonic/NaN guard (lasparser.cpp:1217–1236) matches the comment; no production caller under `src/` (tests and perf only). DLIS/LIS absent samples are explicit NaNs, not a silent zero depth. `TimeDepthTool` rejects non-monotonic keys.
- `resolveManagedPath` / `isSafePathSegment`: `..`, slashes, and symlinks along the managed path are rejected. Map publish checks the horizon segment before `result/<horizon>/vN` (mapversionstore.cpp:931 and 1115) and `pathInsideProject` rejects `..`.
- Raster pyramid tile append checks payload and offset sizes (rasterpyramid.cpp:461–474) and leaves state 0 on failure. ZIP/sfpkg writers size-check `QSaveFile::commit`. `wellfactorfile` goes through `EncodingDetect::decodeText`.

## Not filed
- `LruCache::evictLocked` treats `limit <= 0` as “do not evict” (lrucache.h:242) while `CacheBudgetManager::enforce` passes `0` when one cache is smaller than the remaining overshoot (cachebudget.cpp:145). Incremental `insert()` usually still has one cache larger than that overshoot, and nothing in `src/` calls `loadFromSettings`, so a user-visible miss is not established.
- `parseDepthRange` has no production caller under `src/`. Not filed as a live NaN-depth bug.
- `QString::arg` on map horizon plus version (mapsnapshot.cpp:48) can mis-place `%` in an otherwise safe segment. `isSafePathSegment` still blocks `/` and `..`, so it does not escape the project directory.
- `readWellTableBases` empty result on a small healthy csv is not itself a drop; the loss needs the failed read plus a TVD resolution, which is A-02.
