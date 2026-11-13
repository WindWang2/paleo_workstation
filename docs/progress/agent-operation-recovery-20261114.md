# Agent operation recovery — W5a

W5a provides host-owned immutable schema-2 publication receipts in the existing Catalog SQL transaction, keyed by document/operation and unique output-set ID. Every member version carries the same manifest hash. Healthy rewrites preserve receipts and the store generation; corrupt-primary recovery rotates the generation, so missing evidence in a restored store cannot prove an operation never committed.

`ProjectWriteClaim` is issued only by an acquired project lock and retains the physical cross-process lock while workers hold it. OperationLog v2 freezes original request, resolved plan, exact staging targets and presentation intent. Revision/attempt CAS rejects stale callbacks and immutable-output changes; events retain the latest 128 revisions. Legacy records remain readable and do not claim authoritative absence.

Real linked native verification uses the locked SDK and actual Catalog/DerivedAsset implementations, without storage or publication stubs. `tst_publication_receipt`, existing `tst_catalogstore`, `tst_operation_control` and `tst_algorithm_output_set` all pass. Coverage includes actual subprocess termination/reacquisition, two-process lock denial, receipt/member transaction rollback, replay without another version, corrupted evidence, store rewrite generations, immutable prepared intent and stale-attempt CAS. The 32 CI Source gates plus selftests pass with QGIS 4.2.3 headers; no baselines increased.

W5b remains required: connect this foundation to the production worker/owner commit path, reconcile a lost post-commit response from SQL evidence, and expose scoped operation/result APIs. Publication must remain closed until that path and its crash tests are verified.
