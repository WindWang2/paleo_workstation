# Agent B
Baseline: 42296fd3
Scope: src/domain, src/catalog
Read in full: 18 hot files, including `src/catalog/areawellprojection.cpp`, `src/catalog/areasourceprojection.cpp`, `src/catalog/areasource_delta.cpp`, `src/catalog/areasource_delta_apply.cpp`, `src/catalog/wellavailability.cpp`, `src/catalog/documentsourceprojection.cpp`, `src/catalog/datacatalog_entities.cpp`, `src/catalog/datacatalog_projection.cpp`, `src/catalog/catalogstore_json.cpp`, `src/domain/wellfaciesrequest.cpp`, `src/domain/wellref.cpp`, `src/domain/wellsection.cpp` (tops, zones, `inferSandShale`), `src/domain/documentsource.cpp`, `src/domain/faciescatalog.cpp`, `src/domain/subintervalfacies.cpp`, `src/domain/seismic/timedepthmodel.cpp`, `src/domain/deviationsurvey.cpp` (station ingest and minimum-curvature increment), `src/domain/receiptsemantics.cpp` (well/sample identity). Callers opened outside the slice only to confirm reachability: `src/io/areamodelpublication.cpp`, `src/workflow/projectsetup.cpp`, `src/ui/wellsection/wellsectionscene_columns.cpp`. #1117’s fix lives in `src/workflow/documentsession_wellbores.cpp` and was not re-filed.
Pattern-scanned: `INSERT OR REPLACE` / upsert, `value("id")` as a key, `QString` key joins, revision/`toInt()` defaults, depth `top`/`base` compares, well/horizon/survey id maps, catalog `readAll`/`fromJson` success on a short or missing array.

## Findings

### B-01
- Sev: P1
- Conf: VERIFIED
- Loc: src/catalog/areawellprojection.cpp:150
- Title: Area-well registration rebinds a catalog card by stale name and leaves its LAS links on the new native well
- Bug: A catalog well card keeps its entity id and its asset links, but `areaModelWellId` is rewritten to a different model well. Curve, tops, and wellbore queries that trust that native id then read the other well. The displaced model well gets no card at all.
- Evidence: `registerAreaWells` indexes stored cards by `areaModelWellId` and by raw name once (`areawellprojection.cpp:142-146`) and never updates those maps. Match is `byId.value(id, byName.value(model.name, -2))` (`:150`). A model id that is not yet on a card falls through to the name. `overlayAreaWell` (`:78-82`) copies every model extra key, including `areaModelWellId`, onto that card and `updateEntity` persists it (`:156-157`). The catalog entity id does not change, so links stay put. Because the maps are stale, the next model row can select the same index: processing the renamed old well first and the new well (old name, new id) second steals the card; the opposite order restores the old id and drops the new well. Well names are unique in the model (`vendor/Data_service/src/store/project_store.cpp` wells `ON CONFLICT(name)`), so this is a cross-id name lag, not two model rows with one name. Reachable from publication recovery (`src/io/areamodelpublication.cpp:192`, `registerAreaWells` then `registerAreaSources`) and from generation bootstrap (`src/catalog/areasource_delta.cpp:26`, `:102-103`), which runs when the catalog checkpoint generation is behind the model. Incremental `applyWell` is id-keyed and does not do this.
- Not excluded because: persisted native well id and linked LAS/tops change under a normal publication or generation bootstrap, which is the sibling of the #1117 wellbore reassignment and is not that fix.
- Trigger: Catalog still has well 10 named Alpha. The model has renamed 10 to Gamma and added well 30 named Alpha. `registerAreaWells` runs (model publication recovery, or any generation-mismatch bootstrap).

### B-02
- Sev: P1
- Conf: VERIFIED
- Loc: src/catalog/areasourceprojection.cpp:206
- Title: Survey bootstrap attaches a volume and its horizons to the first same-name survey and keeps the old volume id
- Bug: Two seismic volumes that share a display name become one catalog survey. The second volume’s `seismic_volume` and `horizon` links are stored on the first survey, whose `areaModelVolumeId` is left unchanged. Horizon picks and volume lookups by the second id miss or show the other volume’s card.
- Evidence: `volumes.name` is not unique (`project_store.cpp` `CREATE TABLE volumes(... name TEXT ...)`). `ensureSurvey` returns the first `seismic_survey` whose `areaModelVolumeId` equals `row.id` **or** whose name equals `volume.volumeName` (`areasourceprojection.cpp:205-208`) and does not write the new id or the new name onto that entity. `addModelLinks` then links the source to `surveyIds.value(row.id)` as `seismic_volume` (`:251-252`) and links every horizon to `surveyIds.value(row.parentId)` (`:254-257`; `DataEntry::parentId` is the volume id). The second volume therefore never gets its own card. `bootstrapAreaSources` calls this on every legacy open and on every generation-mismatch bootstrap (`areasource_delta.cpp:100-103`), including the publication recovery path after B-01. The incremental journal path (`areasource_delta_apply.cpp:102-116`) keys surveys by id and does update the name; this bootstrap copy does not.
- Not excluded because: the wrong survey id is persisted and horizon links follow it on a normal first index or generation bootstrap.
- Trigger: Import or publish two volumes that both display as the same name (the table allows it). Open or refresh the area-source projection so `bootstrapAreaSources` runs.

### B-03
- Sev: P2
- Conf: VERIFIED
- Loc: src/catalog/areasourceprojection.cpp:118
- Title: Well-head mapping collapses normalized names and can move or skip the wrong well
- Bug: A coordinate table row for one well is applied to a different well, or dropped, while the call still returns success. The well that is updated is also renamed to the spreadsheet spelling.
- Evidence: Existing wells are keyed only by `DataCatalog::normalizeWellName` (`areasourceprojection.cpp:117-118`), which strips spaces, hyphens, and underscores and lowercases (`datacatalog_entities.cpp:116-126`). `QHash::insert` keeps the last row, so `A-1` and `A1` share one id. The model unique key is the raw name, so both wells can exist. The chosen id is read and, when its wellhead is missing or already equal to the row, `saveWellInfo` writes the row’s X/Y and **replaces `wellName`** (`areasourceprojection.cpp:147-157`). When the survivor already has different finite coordinates the row is `continue`d (`:149-150`) and never applied to the well the user named. `mapped > 0` still returns true (`:162`). Caller is project setup (`src/workflow/projectsetup.cpp:341`).
- Not excluded because: wellhead coordinates and the well’s stored name are wrong on a normal point-mapping import, and a partial success is reported as success.
- Trigger: The area model has two wells whose names differ only by case or by `-`/`_`/spaces. Run project setup with an explicit well/X/Y column mapping that mentions one of those names.

### B-04
- Sev: P3
- Conf: VERIFIED
- Loc: src/domain/wellsection.cpp:22
- Title: Duplicate top names resolve to the first MD, so the formation window and flatten datum use the wrong pick
- Bug: The active formation interval, the flatten shift, and the first correlation link use whichever top with that name appears first, not the shallowest or the one the user picked later. A deeper duplicate pulls the depth window or the flattened section off the horizon the name refers to.
- Evidence: `Well::topMd` returns on the first `t.name == name` (`wellsection.cpp:19-23`). `formationInterval` takes both the active top and the base top through it (`:301-308`). Flatten uses the same lookup (`:191`). `links` also keeps the first right-hand top with that name (`:68-70`). The section view calls `formationInterval` for the highlighted window (`src/ui/wellsection/wellsectionscene_columns.cpp:85`) and `topMd` for per-top positions (`src/ui/wellsection/wellsectionpanel.cpp:975`, `:991`). Nothing in this function rejects a second top with the same name. The tops editor does treat same-depth / duplicate names as issues (`welltopsedit.cpp`), but a section built from an unsorted or duplicated top list never consults that.
- Not excluded because: the drawn formation window and flatten shift are wrong geological depths when a well carries two tops with one name.
- Trigger: Build a section well whose `tops` contain the same horizon name twice at different MDs, then set that name as the active top or the flatten datum.

### B-05
- Sev: P3
- Conf: VERIFIED
- Loc: src/domain/wellsection.cpp:692
- Title: Non-monotonic GR depths invert a lithology run, and the thin-bed merge writes that base back onto the neighbor
- Bug: The inferred sand/shale column drops or shortens a run. A depth that steps backward makes a run whose base is shallower than its top; that run fails the thickness test and its base is copied onto the previous interval, so a real sand or shale stretch disappears from the track.
- Evidence: Direction is only `firstFinite > lastFinite` (`wellsection.cpp:669-672`). Inside a finite block the run top/base are sample or midpoint depths in that index order (`:691-695`), with no monotonic check. A negative `baseMd - topMd` is always `< minThicknessM` (default 0.5). For a block of more than one run the inverted run is deleted and `block[i - 1].baseMd = block[i].baseMd` (`:709-712`). A single inverted run is kept (`:703-704`) and drawn with `y1 - y0` (`wellsectionscene_columns.cpp:473-486`). The provider is the section’s GR fallback when no interpreted lithology is stored.
- Not excluded because: the section paints the wrong sand/shale interval for an otherwise ordinary GR curve that is not strictly monotonic.
- Trigger: Open a well section with no interpreted lithology, GR cutoff fallback on, and a GR depth array that is not strictly monotonic (splice, or a depth that steps back between two lithology changes).

## Covered and clean
- `src/domain/wellfaciesrequest.cpp` and `wellref.cpp`: unknown JSON keys are rejected; model id/version must round-trip through `frozenModelJson`; `model_well_id` must be a canonical decimal string. No extra-key persistence and no missing-version-as-zero overwrite on this contract.
- `src/domain/documentsource.cpp` plus `src/catalog/documentsourceprojection.cpp`: binding revision is written as a string and compared with `>=`, but a missing revision becomes 0 and therefore loses to any positive revision. It does not clobber a newer binding. Tie-break among equal revisions follows append order, which is the newer version after `inheritRelocatedSourceBindings`.
- `src/catalog/areasource_delta_apply.cpp` `applyWell` / incremental `ensureSurvey`: updates follow the model id already cached for this generation, and survey name/id are written onto that entity. Not the bootstrap hole in B-02.
- `src/workflow/documentsession_wellbores.cpp` (#1117): confirm requires the LAS asset’s only well link, `areaModelWellId`, and the projection generation before `confirmSourceWellbore`. The fix is not incomplete on this path. B-01 is the separate catalog rebind that happens before that check.
- `src/domain/seismic/timedepthmodel.cpp`, `deviationsurvey.cpp` (ingest and dogleg), `subintervalfacies.cpp` `makeSandRatioMetrics` / `provisionalSubRange`, `faciescatalog.cpp` `formationRange` / `dominantCode`: inverted or non-finite intervals are rejected or contribute no thickness. `formationRange`’s infinities are not treated as a real window by its in-tree callers.
- `src/catalog/catalogstore_json.cpp` `loadTables` / `fromJson`, `importledger.cpp`, `frameworkstore.cpp`: a truncated JSON file fails the parse and is not opened as success. Link/version rows with corrupt ordinals or unsafe paths are skipped with a warning; the version sequence is still absorbed so the next id is not reused. No short read that returns success after dropping a tail of rows.
- `src/catalog/catalogstore_schema.cpp` `resumeConnection`: a missing or non-numeric `catalog_revision` does not compare equal to a loaded non-zero revision. `setProjectionCheckpoint` refuses a sequence rollback inside the same store and generation.
- `src/catalog/datacatalog_entities.cpp` `wellsMatchingName`: returns every normalized-name hit; it does not pick one. `src/domain/receiptsemantics.cpp` well identity is a display key for receipts, not a catalog primary key.
- `src/domain/seismicclusterproduct.h`: kind predicate only. No assignment store in this slice.

## Not filed
- `catalogWellIds` last-insert on duplicate `areaModelWellId` (`areasourceprojection.cpp:170`): on the generation bootstrap, `registerAreaWells` already aborts when two stored cards share that id. The legacy path can still last-win, but that only happens after B-01’s rebind has already collapsed the cards; not a separate defect.
- `sourceBindingFromJson` ignoring unknown top-level keys: they are dropped on the next `sourceBindingJson`, not written into user fields. Locator maps do keep extra keys; that is the locator bag, and typed relations reject a field count other than 4.
- `recodeIntervals` emitting a part with `top > bottom` when the input interval is already inverted: no production caller.
- `CatalogIndex::versionAdded` defaulting a missing current version number to 1: `CatalogVersion::versionNumber` defaults to 1, and numbers `<= 1` are the documented non-unique band. A 0 is not emitted by the catalog writers read here.
- `QString` joins of the form `agg-` + interval id (`stratigraphicunit.cpp`, `subintervalfacies.cpp`): the aggregate token is prefixed; a collision needs a sub-interval id that is literally `agg-` plus another id. No caller builds ids that way.
- `INSERT OR REPLACE` in `documentsourcestore.cpp`: metadata, outside this slice, and the row is the singleton interpretation payload rather than a well/horizon identity.
- Speculative null crashes, layering, and file-size nits.
