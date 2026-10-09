# Offline Office preview fixtures

Synthetic documents used by `tst_officepreview` with the source-built Calligra
renderer. The DOCX source is `testdata/project_area/mini_report.docx`; XLSX is
`tests/fixtures/outsource/coordinates.xlsx`. `sample.fodp` is a minimal synthetic
slide. DOC, XLS, PPT and PPTX were originally saved from those sources using
LibreOffice solely to prepare valid input fixtures; the application preview
excludes LibreOffice. `long.xlsx` replaces the first worksheet with 250 numeric
rows to verify pagination and navigation beyond the first page. `inline.xlsx`
contains inline text without a styles part. The Office XLSX fixtures declare
worksheet/shared-string content types explicitly, as required by OOXML.

Build `vendor/fetch-calligra.sh`, then run in the isolated CTest environment:

```bash
ctest --test-dir build -R '^tst_officepreview$' --output-on-failure
```

All six format cases work with Qt offscreen; there is no X11/foreign-window
requirement. `PALEO_OFFICE_FIXTURE_DIR` optionally selects another directory
containing `sample.doc`, `sample.docx`, `sample.xls`, `sample.xlsx`, `sample.ppt`,
`sample.pptx`, `long.xlsx`, and `inline.xlsx`. Runtime cases skip only when the optional renderer
has not been built; the local acceptance run requires all cases to execute.
