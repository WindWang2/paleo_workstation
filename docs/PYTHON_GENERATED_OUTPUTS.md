# Controlled report, table and image outputs

`paleo.python_outputs` / `python.generated_outputs` is a distinct capability with
implementation `python-generated-outputs.v1`. Existing curve REPORT v1, numeric
table REPORT v1, seismic REPORT v1 and O1 report/table operations retain their own
IDs, strict validators and restorers. No stored operation is silently upgraded.

Choose one explicit `inputs.input_kind`: `numeric_table` or `seismic_trace`.
The remaining input fields use the corresponding unchanged
[table](PYTHON_NUMERIC_TABLE_INPUT.md) or
[trace](PYTHON_SEISMIC_TRACE_INPUT.md) selection. The metadata-only query topics
`numeric_table_projection` and `seismic_trace_descriptor` return an exact
`input_ref`; the caller must already know the authorized source and table/volume
ID. Add `input_kind` to that reference and explicitly choose the bounded window.
The response does not certify source integrity or execution readiness.

Fresh prepare/compute parameters contain exactly `code`, `output_table` and
`output_image`. Code remains at most 16 KiB UTF-8. The table declaration uses the
unchanged [O1 contract](../vendor/agent-python/NUMERIC_OUTPUT.md); the image uses
[straight RGBA8](../vendor/agent-python/IMAGE_OUTPUT.md), with width and height in
1..1024. The host freezes these declarations, encoder identity and policy in
`paleo.python-generated-output-profile.v1`. Tools cannot provide paths, MIME,
runtime roots, SQL, host context, configuration or encoder settings.

The child's stdin remains the chosen typed input metadata/binary frame. Output
declarations are supplied in the tool request and frozen by the host; the program
must emit matching declarations. They are not an additional file or filesystem
entry for the child. Stdout uses frame v3 with exactly ordered REPORT, TABLE and
raw IMAGE segments. REPORT body validation first checks the actual input kind,
then binds code/runtime/source/ReadCut evidence. The host validates every segment,
encodes numeric CSV and PNG, and only then writes the complete private output set.

REPORT is at most 256 KiB; CSV and PNG are at most 4 MiB each. Raw and final output
groups also obey the 12-MiB protocol and the smaller approved policy ceiling.
Dimensions and byte arithmetic are checked before image allocation. The collector
only lowers the approved stdout limit; it grants no extra permission or resource
limit. Host staging/encoding reservations are checked separately from child RSS;
they are conservative budgets, not a hard bound on the whole GUI process.
Maximum dimensions do not promise that every incompressible image fits PNG's cap.

The inner frozen plan `paleo.python-generated-outputs-operation.v1` retains the
chosen input contract and is wrapped in the existing managed-environment
envelope. Both input kinds reuse their original selector, SDK materializer,
InputBundle and validators; execution uses the shared generated worker and
TaskService. Source authentication retains the complete SHA anchor and range
proofs, with initial full-scan and subsequent range bytes reported separately.
No second interpreter, task queue, operation log, output store or publisher exists.

All three business ports are required. A malformed segment, cancellation, encoder
failure, budget error or even the third private-file write failing returns no
business output group; bounded private diagnostics remain available. Prepare and
compute create no formal Catalog version. Publish accepts only the original
operation reference with empty parameters and uses the existing authorized atomic
output-set/Catalog/receipt path. Retries and recovery inspect the same durable
facts. Publication does not rerun the program or require the original source file.

Compute/resume checks the frozen PNG encoder against the actual current Qt
compile/runtime identity and fixed settings. Publication of already authenticated
PNG bytes validates the historical profile structure and consumes those bytes,
without invoking or requiring the current encoder. Tests in one binary do not
claim that an actual cross-Qt upgrade has been exercised.

REPORT and table/image metadata retain input, code, runtime and output-profile
provenance; IMAGE also records the authenticated raw RGBA SHA and host encoder.
The PNG is a nonspatial report artifact. It declares no CRS/extent, creates no GIS
layer automatically and is not accepted geological interpretation. Generated
numerical values and pixels are not independently recomputed by the host.

Execution still requires explicit generated-code permission and a successful real
strong-backend probe. Pi is an optional host-controlled orchestration engine; it
uses the same schemas, admission, stable invocation bindings, operation facts and
publication path. Installing Python or Node does not enable either engine.
Shell, arbitrary CLI/SQL/HTTP, online package installation and persistent REPL
remain deferred. Actual platform/build/execute evidence belongs to the
[O2 progress record](progress/python-generated-outputs-o2-20261115.md).
