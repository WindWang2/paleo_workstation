# Offline Office preview fixtures

Synthetic documents used by `tst_officepreview`. Session tests use a local
editor stub, or `PALEO_OFFICE_EDITOR` when it points at a ranuts/document
build with `index.html`. The DOCX source is `testdata/project_area/mini_report.docx`; XLSX is
`tests/fixtures/outsource/coordinates.xlsx`. `sample.fodp` is a minimal synthetic
slide. DOC, XLS, PPT and PPTX were originally saved from those sources using
LibreOffice solely to prepare valid input fixtures; the application preview
excludes LibreOffice. `long.xlsx` replaces the first worksheet with 250 numeric
rows to verify pagination and navigation beyond the first page. `inline.xlsx`
contains inline text without a styles part. The Office XLSX fixtures declare
worksheet/shared-string content types explicitly, as required by OOXML.

The session tests do not need `vendor/fetch-ranuts-document.sh`. Run them in
the isolated CTest environment:

```bash
ctest --test-dir build -R '^tst_officepreview$' --output-on-failure
```

Qt offscreen does not create a WebEngine view; the widget test then expects the
system-browser fallback for the same `127.0.0.1` address. `PALEO_OFFICE_FIXTURE_DIR`
optionally selects another directory containing `sample.doc`, `sample.docx`,
`sample.xls`, `sample.xlsx`, `sample.ppt`, and `sample.pptx`.
