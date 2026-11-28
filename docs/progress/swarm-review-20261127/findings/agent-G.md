# Agent G
Baseline: 42296fd3
Scope: src/qgis
Read in full: the controlling functions in the paths below (not every file in src/qgis). Hot files: paleomapfacies.cpp, qgiscanvascontroller.cpp, previewmapcanvas.cpp, projectmapreference.cpp, seismicsectiontool.cpp, qgislayerservice.cpp (instantiate), layoutexport.cpp (thickness extent), crossplotmaplink.cpp, mapfeaturepicker.cpp, previewidentify.cpp (toLayerCrs), coordinatetoolsdialog.cpp (isValid throw), wellattributestore.cpp (upserts), mappingartifactwriter.cpp (raster/gpkg writers), unitraster.cpp, qgisprocessingservice.cpp (run/lookup), qgisprojectservice_async.cpp, evidencemapadapter.cpp, wellsectionmapband.cpp, geostatrasterio.cpp, factorcontour.cpp, dataexportwriter.cpp, agentmapdisplayadapter.cpp, qgiseditingservice.cpp.
Pattern-scanned: src/qgis for NoThreading/algorithmById/runBlocking, .partial/GDALCreate/RasterIO/CreateFeature, snap tolerance units, addMapLayer/removeMapLayer, static_cast on layers, transformPoint/mapExtent/CrsType::Engineering, well_id+horizon upserts, geometry written without a transform.

## Findings

### G-01
- Sev: P1
- Conf: HIGH
- Loc: src/qgis/paleomapfacies.cpp:119
- Title: Facies-map normalize writes source coordinates into the project CRS when the transform is not valid
- Bug: Importing a polygon paleomap into a CRS-registered project saves `facies.gpkg` labeled with the project CRS, but the rings are still the source coordinates. Later maps treat local metres (or an unlabeled file) as that projected CRS.
- Evidence: `normalizeFacies` builds `QgsCoordinateTransform` with an empty `QgsCoordinateTransformContext` (not the project context that holds the engineering operation), then `geometry.transform(transform)` and ignores the result (`paleomapfacies.cpp:143`). The feature is still `addFeature`d into a writer created with `destinationCrs` (`:121`, `:145`). `coordinatetoolsdialog.cpp:203` has to throw by hand when `!transform.isValid()` because `transform()` does not. Sibling writers refuse that case (`constraintcrs.cpp:57`, `factorwellcoordinates.cpp:16`, `projectmapreference.cpp:186`). Caller `src/workflow/paleomapimport.cpp:48` passes `targetCrs` whenever georeference kind is `crs`, and `:180` publishes a non-empty return. The `QgsCsException` path returns empty and the caller deletes the directory; the invalid-transform path does not throw and is stored.
- Not excluded because: persisted geometry CRS is wrong, not a missing feature or a layering nit.
- Trigger: Normalize facies (default on paleomap import) when the source layer CRS differs from the project CRS and the empty context has no operation — local engineering grid or a CRS-less file into a `kind=crs` project.

### G-02
- Sev: P2
- Conf: VERIFIED
- Loc: src/qgis/qgiscanvascontroller.cpp:276
- Title: Zoom-to-layer keeps the layer's own coordinates when the canvas CRS cannot be reached
- Bug: The canvas jumps to the layer's raw numbers interpreted in the canvas CRS (engineering metres shown as degrees, or a projected canvas parked on a local-metre box). The comment just above says this is the EPSG:3857 failure mode they meant to avoid.
- Evidence: `mapExtent` returns an empty rectangle when `!transform.isValid()` (`projectmapreference.cpp:199`). `zoomToLayer` then does `ext = sourceExtent` with no `CrsType::Engineering` test (`qgiscanvascontroller.cpp:276-283`). `PreviewMapCanvas::zoomToLayer` does the same (`previewmapcanvas.cpp:443-446`). The intended keep is only the engineering-grid fallback in `layoutexport.cpp:631-634` (invalid transform AND project CRS engineering AND `paleoCrsProvenance=confirmed_project_crs`). `zoomToFullExtent` / `PreviewMapCanvas::fullExtent` already skip a null mapped extent instead of substituting it. Callers include `appcontext_layers.cpp` (wells) and the layer-tree zoom actions.
- Not excluded because: the view moves to a wrong place on a normal zoom action.
- Trigger: Zoom to a layer whose CRS has no operation to the current canvas CRS (local-grid wells or a facies raster on a projected canvas, or a WGS84 preview on an unregistered engineering canvas).

### G-03
- Sev: P2
- Conf: VERIFIED
- Loc: src/qgis/seismicsectiontool.cpp:63
- Title: Section-tool well snap drops every well when the transform fails, including an engineering canvas
- Bug: Activating map section capture yields an empty snap set, so wells cannot be picked. The comment says an unregistered project passes the local-grid wells through; the condition does not.
- Evidence: Wells are `surfaceX/Y` (`sectionworkbench.cpp:383-388`). `setSnapWells` treats `link->sourceCrs()` as that CRS (`paleomainwindow_sections.cpp:333`, `seismicsectiontool.cpp:56`). Passthrough happens only when the canvas CRS is invalid or WKT-equal to that seismic CRS (`:57-61`). Any `transformPoint` failure `continue`s (`:66-68`) — `transformPoint` is false when `!isValid()` (`projectmapreference.cpp:186`). There is no branch that keeps the unprojected points when `destination.type()==Engineering`, which is the fallback `layoutexport.cpp:631` uses for grid metres. A projected canvas that truly cannot transform should drop them; an engineering canvas should not.
- Not excluded because: a normal “draw section on the map” action loses well snap, and the in-file comment describes the missing fallback.
- Trigger: Open a volume whose coordinate-system text is not identical to the canvas CRS, on an engineering (unregistered) project, then start section capture. Transform from that label to the local grid fails and every well is removed from `m_snapWells`.

### G-04
- Sev: P2
- Conf: VERIFIED
- Loc: src/qgis/qgislayerservice.cpp:302
- Title: CRS-less layers are labeled EPSG:32650 and “confirmed project CRS” even when the project is the engineering grid
- Bug: A file with no SRS is shown, identified, and snapshotted as UTM zone 50. Layout extent math then treats those same numbers as engineering metres (`layoutexport.cpp:631`), so the frame and the layer CRS disagree. `vectorSnapshot` persists whatever `layer->crs()` is.
- Evidence: After `assignStoreCrsIfMissing` (constraints only), any still-invalid layer gets `EPSG:32650` unless `georeference.kind=="crs"` (`qgislayerservice.cpp:302-311`), and the provenance is set to `confirmed_project_crs`. It never reads `project->crs()`. Unregistered projects stay on `DataCatalog::localGridCrsWkt` (`qgiscanvascontroller.cpp:124-125`). `confirmedSurveyCrs` (`projectmapreference.cpp:261`) already returns the project CRS in that case; instantiate does not use it. `layoutexport.cpp:629-634` then keeps the raw extent only because of that provenance flag, while the layer itself remains 32650 so the map item still has no engineering operation.
- Not excluded because: this is a wrong CRS on a layer the user opens, not an unfinished migration.
- Trigger: Instantiate any declared raster or vector that still has no CRS after `restoreRasterCrs` / `restoreVectorCrs`, in a project that is not a `kind=crs` georeference (the default engineering project).

### G-05
- Sev: P2
- Conf: HIGH
- Loc: src/qgis/crossplotmaplink.cpp:38
- Title: Crossplot locate and highlight keep sample coordinates when the canvas CRS differs and no operation exists
- Bug: “Locate sample” and the selection highlight succeed and draw the point in the source CRS numbers on the canvas. A local-grid sample on a projected canvas is placed at those metre values, not at the projected location, and the UI does not report failure.
- Evidence: `coordinate` and `highlight` call `QgsCoordinateTransform::transform` whenever the CRS objects differ, and the only failure path is `catch (QgsCsException)` (`crossplotmaplink.cpp:37-45` and `:118-125`). They never test `isValid()`. `coordinatetoolsdialog.cpp:203` throws explicitly in that case because the transform call will not. `mapreference::transformPoint` returns false instead (`projectmapreference.cpp:186`). Caller `src/app/crossplotcontroller.cpp:413` and `:430`.
- Not excluded because: the map highlight is a wrong location reported as success.
- Trigger: Crossplot locate/highlight when `SampleSet.grid.crs` (or the local grid, if empty) has no operation to the canvas CRS — engineering sample grid, projected canvas, or the reverse.

### G-06
- Sev: P2
- Conf: HIGH
- Loc: src/qgis/mapfeaturepicker.cpp:32
- Title: Feature pick measures layer coordinates against a canvas-unit radius when the CRS transform fails
- Bug: Click or hover on a vector whose CRS cannot be transformed selects the wrong feature or looks like a miss. Identify was written to refuse this case; the picker does not.
- Evidence: `candidates` sets `transform` from CRS inequality only, then `transformBoundingBox(..., Reverse)` and `geometry.transform(toMap)` with no `isValid()` check and no test of `GeometryOperationResult` (`mapfeaturepicker.cpp:29-40`). The radius is canvas map units (`:26`). `previewidentify.cpp:35-53` documents the same situation: if the operation cannot be built, do not query the layer with canvas coordinates, because a silent wrong hit is worse than a miss. Callers catch `QgsCsException` only (`mapfeaturepicker.cpp:82-100`); an invalid transform does not take that path (`coordinatetoolsdialog.cpp:203`).
- Not excluded because: edit/select uses this picker (`createSelectionTool` in the same file) on ordinary mixed-CRS layers.
- Trigger: Select or hover the current vector layer when its CRS differs from the canvas and the project transform context has no operation (engineering layer on a projected canvas, or the reverse).

## Covered and clean
- `qgisprocessingservice.cpp` `run` / `lookupAlgorithm` / `paleoAlgorithmIds`: registry reads take `m_registryMutex`; `NoThreading` hops to the main thread with `BlockingQueuedConnection`. No second unlocked `algorithmById`.
- `qgisprojectservice_async.cpp`: background `QgsProject::read` is a private project, with the documented handoff. Not a live-project read from the worker.
- `mappingartifactwriter.cpp` raster `.partial` and `pointGeoPackage`: failure deletes the partial or leaves it inside a `QTemporaryDir`; commit uses `paleoReplaceFile`.
- `geostatrasterio.cpp`, `factorcontour.cpp`, `seismicclusterpatches.cpp` `polygonize`, `seismicpatchassignment.cpp` `writeSnapshot`, `provisionalvector.cpp`: staging or `qScopeGuard` remove on failed GeoTransform / feature add.
- `wellattributestore.cpp` `seedFactors`: duplicate `(well_id, horizon)` keeps the first fid and registers a newly added fid so the same batch does not insert a second row. `mergeFormations` overwrites the index, but `validate` already rejects duplicate keys before the edit.
- `wellsectionmapband.cpp`, `previewidentify.cpp` `toLayerCrs`, `mapextentwidget.cpp`, `agentmapnavigation.cpp`: invalid transforms abort instead of painting source coordinates.
- `agentmapdisplayadapter.cpp`: visibility/opacity/style do not `addMapLayer`. `previewmapcanvas.cpp` `forgetLayer` compares `QObject*` and does not `static_cast` the dying layer.
- `qgiscanvascontroller.cpp` `nativeSnappingConfig` and `seismicsectiontool.cpp` `snapToleranceMapUnits`: default and the editing toolbar store pixels and the section tool multiplies pixel tolerance by `mapUnitsPerPixel`. Layer-unit configs are not produced by that toolbar.

## Not filed
- `unitraster.cpp` `writeInput` leaves an incomplete GeoTIFF on SetGeoTransform / SetProjection / RasterIO failure, but the only production caller (`predictionworkflow.cpp`) puts it in a `QTemporaryDir` that is destroyed on the failure return.
- `dataexportwriter.cpp` does not delete a failed `data.gpkg`; `dataexport.cpp` removes the destination directory, and geological export writes into a temp dir that is not renamed to the user destination on failure.
- `paleomapfacies.cpp` `QgsCsException` return does not itself `QFile::remove`, but `paleomapimport.cpp` `removeRecursively`s the destination directory when the result is empty.
- `mergeFormations` `QMap::insert` keeps the last duplicate `(well, formation)`; `validate` rejects that table before merge, so a normal call never updates the last row.
- Processing-registry pointer use after the mutex scope: no `removeProvider` caller in src, so a dangling `QgsProcessingAlgorithm*` is not a current path.
- `zoomToFullExtent` skipping a layer whose `mapExtent` is null (no raw-coordinate fallback) — that is the safer behavior.
- `setSnapWells` dropping wells when the canvas CRS is a real projected CRS and the operation fails — that drop is correct; only the engineering-canvas case is filed.
