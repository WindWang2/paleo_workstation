# Calligra 26.08.2 preview patches

The source archive and upstream commit are pinned in `vendor/manifest.json`.
`vendor/fetch-calligra.sh` applies these tracked patches idempotently; source
files are otherwise unchanged except for the final Paleo renderer CMake include.

- `xlsx-optional-styles`: valid XLSX cells may omit style information. Guard the
  nullable default cell format before reading its number-format flag. Without
  this guard, an absent styles part crashes `XlsxXmlWorksheetReader::read_c`.
- `xlsx-inline-text`: read inline `<is>` strings, including text runs, and skip
  phonetic annotations. Common workbook writers use inline strings instead of
  a shared string table. Preserve text and existing cell formatting; per-run
  inline font differences remain an upstream rendering limitation.

Integration coverage: `tst_officepreview` uses a workbook without styles, a
250-row workbook spanning pages, and an inline-string-only first worksheet.
The helper still validates the file through Calligra and never edits the source.
