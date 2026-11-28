# Agent C
Baseline: 42296fd3
Scope: src/algorithms
Read in full: kriging.{h,cpp}, linsolve.{h,cpp}, contourlevels.{h,cpp}, velocitymodel.{h,cpp}, depthbatch.{h,cpp}, evidencesynthesis.cpp, sgs.h, neighborhood.h (dedupe + queryNearest), seismicattr.h
Pattern-scanned: NaN comparisons, row-scale variance (`scaleKrigingSystemRows` / `systemScale`), neighborhood and realization budgets, contour anchors, ms/m unit factors (2000, /1000), raster index, silent sample drops. Hot files opened in part: sgs.cpp, sgs_internal.h, sgs3.cpp, cokriging.cpp, variogram.cpp, localidw.cpp, upscale.cpp, propfill.cpp, lowfreq.cpp, bandlimit.cpp, sparse.cpp, seismicattr.cpp, faciespolygonize.cpp, distancetransform.cpp, horizontrack.cpp, faultpath.cpp, ensemblestats.cpp. Callers opened only to test reachability: workflow/geostatatomoperation.cpp, geostatatomcontract.cpp, constraintfactorjobs_geostat.cpp, algorithmadapters.cpp, depthconversionworkflow.cpp, tests/tst_geostat_cokriging.cpp.

## Findings

### C-01
- Sev: P2
- Conf: VERIFIED
- Loc: src/algorithms/geostat/kriging.cpp:239
- Title: maxPoints<=0 bypasses the kriging matrix budget and solves the full sample set per cell
- Bug: A kriging (or cokriging) job that asks for every sample allocates an (n+1)² system on every grid cell once n exceeds 2895, or on every cell inside a search radius with no K cap. A few thousand wells is hundreds of megabytes per cell and an O(n³) factorization; the task thread dies on bad_alloc or never finishes. The grid is not a capped nearest-K surface.
- Evidence: `kMaxGlobalFactorSamples` (2895) only skips the one-time LU (`kriging.cpp:173-175`). `neighborhood()` then sets `k` to `index.size()` whenever `maxPoints<=0` (`kriging.cpp:239-240`) and `solveOrdinaryKriging` allocates `n1*n1` with no second cap (`kriging.cpp:31-33`). `searchRadius>0` also skips the global factor, so the same uncapped K applies inside the radius. `clampParams` leaves `maxPoints<=0` untouched (`kriging.cpp:113-118`). Cokriging repeats it: `clampNeighborhood` maps `<=0` to 0 (`cokriging.cpp:36-38`) and `solveCoreAt` uses the full primary and secondary counts (`cokriging.cpp:103-114`). The factor workflow passes the parameter through (`constraintfactorjobs_geostat.cpp:1568`, `620`) and its QC states that kriging/cokriging `<=0` means every sample. `localidw.cpp:390-400` already caps this mode at a 512 MiB / ~8192-neighbor budget because 20000 wells is a 3.2 GB matrix; the geostat entry points do not.
- Not excluded because: this is a reachable memory blow-up on a documented call, not a style or baseline nit. SGS is not the same hole: it clamps K to [1, 64].
- Trigger: Geostat kriging or cokriging with `maxPoints`/`maxPrimary` <= 0 (the “all samples” switch) and more than 2895 finite wells, or the same switch plus a search radius that still covers thousands of wells. Default K=16 does not hit it.

### C-02
- Sev: P4
- Conf: VERIFIED
- Loc: src/algorithms/geostat/cokriging.cpp:657
- Title: Constrained kriging variance multiplies an already-unscaled λ·γ by the row scale
- Bug: `ConstrainedKrigingSolver::solveAt` returns `ok=true` with a kriging variance too large by about the variogram sill (or too small when the sill is below 1). The estimate and the weights are the unscaled solution and stay right; only the variance field is wrong.
- Evidence: Row scaling divides the copy `b`, not `rhs` (`cokriging.cpp:596-605`). The dot product then uses the original `rhs` (`cokriging.cpp:657`), which is already Σλᵢγᵢ₀, and the next line multiplies by `systemScale` again (`cokriging.cpp:662-663`). Ordinary kriging and cokriging dot the scaled right-hand side and multiply once (`kriging.cpp:76-82`, `cokriging.cpp:201-214`). `systemScale` is the max |Γ| entry, so it tracks nugget+sill whenever any pair is separated. Tests call `solveAt` (`tests/tst_geostat_cokriging.cpp:305`, `322`) but never check the variance magnitude. No production caller constructs `ConstrainedKrigingSolver` (only this .cpp and that test).
- Not excluded because: the controlling formula is local, and it is the row-normalized variance bug, not a missing feature. Severity is P4 because the production caller list is empty.
- Trigger: Any `solveAt` on two or more separated samples with a positive sill. `systemScale==0` (a single sample, Γ block all zeros) happens to stay correct.

## Covered and clean
- `ordinaryKriging` / `localidw` kriging branch / `solveCoreAt`: row scale is applied to the right-hand side that the variance dots, then multiplied back once. Agent atom defaults (`MAX_POINTS` 16 in [1, 64], `RANGE>0`, seed parsed as a decimal string) do not hit C-01.
- SGS / SGS3: range and non-finite nugget/sill/azimuth/ratio are rejected; K clamped to [1, 64]; realization bytes capped at 4 GiB; seed is SplitMix64-mixed and stable for a fixed seed. All-nodata from a tiny search radius is `Status::Ok` with `solverFailures` set; the agent worker rejects `finite_cells<=0` before publish.
- `VelocityModel` interval and V0–k depth: two-way ms uses the 2000 factor consistently; in-range interval depth does not extrapolate; spatial IDW stays NaN when every well is outside its knots. `convertTimeGridToDepth` refuses a size mismatch instead of treating missing samples as 0 ms. The only production reader caps grids at 4e6 cells, so the `rows*cols` int multiply is not reachable there.
- Contour levels: anchor is `ceil((valueMin/step) - eps)*step`, count is `floor(span/step+1e-10)+1`, and `kMaxContourLevels` blocks the huge allocation. No skipped or duplicated level on the step grid the code actually builds.
- Seismic complex trace: instantaneous frequency divides by `dt` in seconds (`sampleIntervalMs/1000`). Sweetness’s negative-frequency floor is the documented `fMin`. Semblance NaN in the window stays NaN.
- Upscale `layerOfFraction`, evidence synthesis (non-finite weights skipped, boundary points not double-counted), and checkshot ft→m / s→ms live outside this slice’s defect set.

## Not filed
- SGS `maxPoints<=0` clamps to 1 rather than “all samples”. QC records the effective K (`constraintfactorjobs_geostat.cpp:594-596`). Harsh, but not a silent success.
- `fitV0kTwoKnots` through-origin fallback misses both knots when the first knot is on an axis (`t1<=0` or `z1<=0`) while the comment says the fit is exact (`velocitymodel.cpp:65-73`). The only production collector drops `timeMs<=0` and `depth<=0` before `fit`, so that input does not arrive.
- Interval extended depth above the first knot uses the (0,0)–(t1,z1) velocity (`velocitymodel.cpp:373-383`). Documented #719 stand-in, and only when some other well is still inside its knots.
- `legacyControlDepth` returns MD when TVD is absent. The header says it is not an MD→TVD conversion; the substitution is in the workflow collector.
- Contour `spinRound` duplicate/skip: `spinDecimals` keeps the printed step on a grid the level loop can distinguish. No controlling counterexample.
- Kriging/SGS all-nodata from an empty search radius: same nodata contract as a radius miss, and the agent atom fails the run when `finite_cells<=0`.
