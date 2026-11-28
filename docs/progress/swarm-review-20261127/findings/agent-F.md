# Agent F
Baseline: 42296fd3
Scope: src/ai (entire); agent-facing src/workflow and src/app (agent, aichat, llm, operation, capability, plan, python, sandbox, pi, tool contract, session archive, context budget, onnx)
Read in full: tileinference.{h,cpp}, tileprobability.cpp, unitonnxtools.cpp, unitonnxoperation.cpp, llmclient.cpp (issue/finish/non-stream), chatsession.cpp path/load/save, chatsessionarchive.h + budget functions, aichatcontextprojection.cpp, aichattoolrunner_results.cpp, chatresultstore.cpp encode/read, capabilityrecovery.cpp, aichatcontroller_toolloop.cpp, toolresultprotocol.cpp, geostatictools.cpp, geostatatomcontract.cpp decode, geostatatomoperation.cpp kernel + inherit, remotepredictrouter.cpp, hostcallcontext.cpp, domaintools.cpp effects/validate, capabilityhost.cpp prepare, agentcapabilityinspection.cpp describe effects, operationtools.cpp, python sandbox launch (windows + linux strong profile) and pythongeneratedworker.cpp
Pattern-scanned: class/channel layout, Authorization/RedirectPolicy, archive path and quota, callerScope, finite_cells, tool ok vs publish/cancel, result cache drop, sandbox network/process/filesystem, capability effects vs runtime

## Findings

### F-01
- Sev: P2
- Conf: HIGH
- Loc: src/ai/remotepredictrouter.cpp:216
- Title: Remote predict follows cross-host redirects and will persist that host's class grid
- Bug: A configured remote-predict endpoint that answers 3xx is followed. On Qt 6 the default policy is NoLessSafeRedirectPolicy (https→https crosses hosts; 307/308 resends the POST body). The final HTTP 200 body is parsed as the facies/class grid and MappingWorkbench emits it as method "remote". Seismic samples can be delivered to the redirect target, and that target's cells are what gets written.
- Evidence: healthCheck (src/ai/remotepredictrouter.cpp:216) and predict (src/ai/remotepredictrouter.cpp:246) build QNetworkRequest with no RedirectPolicyAttribute. Every other agent HTTP client sets ManualRedirectPolicy because "Qt 默认会跟随跨源重定向" (src/ai/chat/llmclient.cpp:290-294, src/ai/wellfaciesservice.cpp:241, src/workflow/llmmodelinventory.cpp:82). Finished handler accepts any status==200 payload whose cells array matches the grid (src/ai/remotepredictrouter.cpp:165-208) and onPredict emits completed (src/ai/remotepredictrouter.cpp:365-381). Caller: assembleRemotePrediction (src/app/aiwiring.cpp:41-46) and MappingWorkbench::start via m_remote->start (src/workflow/mappingworkbench_predict.cpp:453-456).
- Not excluded because: this is a live prediction client, not the deferred Python shell/network profile, and it is not the already-fixed LlmClient/WellFacies path.
- Trigger: PALEO_REMOTE_PREDICT_URL points at an https service that returns 302/307 (open redirect, moved endpoint, or a hostile response). The user runs seismic or well remote prediction.

### F-02
- Sev: P2
- Conf: VERIFIED
- Loc: src/workflow/aichatcontroller_toolloop.cpp:192
- Title: A tool result that cannot be cached is still reported to the model as success
- Bug: When the native tool succeeded but the paleo.tool-result.v1 body does not fit the reply quota and ChatResultStore::put refuses it, data and resultRefs are removed while status stays "succeeded". Pi and the UI are still told ok=true, with retryAdvice do_not_repeat_execution, so the model keeps a successful call and cannot read the payload back.
- Evidence: encode builds the envelope with the caller's ok flag (src/services/toolresultprotocol.cpp:55-61), then on overflow calls put. put fails with result_too_large (raw JSON > 1 MiB) or result_cache_full (128 handles or 8 MiB). transportResult then deletes data (src/workflow/chatresultstore.cpp:128-140, src/services/toolresultprotocol.cpp:97-104) and does not insert resultRefs when the reference is empty. onToolFinished only clears deliveredOk for paleo.read_result plus tool_reply_budget_changed (src/workflow/aichatcontroller_toolloop.cpp:189-195) and passes that flag to Pi (src/workflow/aichatcontroller_toolloop.cpp:219, src/workflow/pichatbridge.cpp:288-290). The comment at toolloop.cpp:190 says a result that was not delivered must not stay ok=true; that exception is only implemented for the read tool.
- Not excluded because: this is the current H7 delivery path, not a missing feature. The call did run; the model is told a false delivery fact.
- Trigger: A successful tool returns a JSON object larger than the frozen reply quota (minimum 768 bytes, cap 8 KiB) after the session cache is full, or a single result above 1 MiB. Typical after many large facies/query/operation payloads inside the 30 minute TTL.

### F-03
- Sev: P2
- Conf: VERIFIED
- Loc: src/ai/chat/geostatictools.cpp:48
- Title: Geostat publish that follows the tool text is rejected, and a filled parameters object cannot publish
- Bug: paleo.geostat_kriging and paleo.geostat_sgs tell the model to omit parameters on compute-only inherit and to pass only source_operation_id on publish. The declared schema marks parameters required, so omission never reaches the host. A non-empty parameters object, which is what "required" pushes the model to send, is rejected as mixed_source_operation_inputs. The only publish that runs is parameters:{} which neither the description nor the required flag describes. Catalog publish of an ordinary-kriging or SGS result therefore fails on the calls the contract text invites.
- Evidence: parameters is required:true and the description says compute-only inherit should omit it (src/ai/chat/geostatictools.cpp:43-51); inputs text says publish's only input is source_operation_id (src/ai/chat/geostatictools.cpp:198-203). validateParameters rejects a missing required key before dispatch (src/ai/chat/domaintools.cpp:257-260, called from dispatchAiTool at src/ai/chat/domaintools.cpp:653). Host publish accepts the call only when parameters.isEmpty() and inputs.size()==1 (src/workflow/capabilityhost.cpp:213-214). decodeGeostatAtomRequest rejects publish_intent publish outright if it reaches the atom (src/workflow/geostatatomcontract.cpp:655-656), so publish cannot be repaired inside the atom either.
- Not excluded because: this is the I1 agent atom (#1081), not an unmigrated legacy command. The failure is on the documented publish step, which is the only catalog write for these tools.
- Trigger: The model calls paleo.geostat_kriging or paleo.geostat_sgs with publish_intent publish and inputs {source_operation_id}, either omitting parameters (schema) or repeating the prepare parameters (host).

### F-04
- Sev: P3
- Conf: VERIFIED
- Loc: src/workflow/agentcapabilityinspection.cpp:837
- Title: capability_inspect describes geostat publish tools as Read while they write catalog rasters
- Bug: describe of paleo.geostat_kriging / paleo.geostat_sgs reports declared effects as Read only. prepare needs a project write lock (PrivateStage) and publish also CatalogPublish and Presentation. A caller that trusts the lifecycle section will treat a layer-writing tool as read-only. check with an explicit publish_intent does use effectsForArguments; describe does not.
- Evidence: capabilityEnvelope never assigns effects, so the default is host::Read (src/ai/chat/domaintools.h:48, src/ai/chat/geostatictools.cpp:19-58). describe's lifecycle section calls describeToolEffects(spec), which prints spec.effects and only a prose note that derivation exists (src/workflow/agentcapabilityinspection.cpp:240-246 and :836-837). Runtime publish effects come from computeEffects("publish") because isAiCapabilityTool includes the geostat aliases (src/ai/chat/domaintools.cpp:202-208 and :626-630, src/services/hostcallcontext.cpp:46-49). The worker then writes OUTPUT.tif / VARIANCE.tif or STD.tif into the catalog (src/workflow/geostatatomoperation.cpp:228-266).
- Not excluded because: the effects section is the machine-readable policy surface of paleo.capability_inspect. The prose description mentions catalog versions, but the declared mask the inspect tool returns does not.
- Trigger: paleo.capability_inspect mode describe, target_kind tool, target_id paleo.geostat_kriging or paleo.geostat_sgs, sections including lifecycle.

## Covered and clean
- tileinference softmaxGrid class_index path keeps argmax for label < 255 instead of treating the channel count as the label vocabulary (src/ai/tileinference.cpp:118-136). Inner tiles do not overlap, so stitch's first-visit argmax is the persisted value. unit.onnx_predict rejects semantics other than logits/probabilities and checks outputShape[1] against the manifest class count (src/workflow/unitonnxcontract.cpp:111-119, :256-257). The old "every class_index becomes nodata" bug is not still in these two paths.
- Ordinary kriging / SGS atom fails geostat_no_finite_cells when finite_cells <= 0 before it sets ok (src/workflow/geostatatomoperation.cpp:276-281). Kernel Status::Ok with an all-NaN grid is not published.
- LlmClient and WellFaciesService set ManualRedirectPolicy and do not treat an empty or zero-event body as a finished answer (src/ai/chat/llmclient.cpp:293-294, :507-510, :514-538).
- Chat session ids reject empty, ".", "..", slash, backslash, colon, and NUL (src/ai/chat/chatsession.cpp:91-100). Load requires the canonical parent to be the session root and refuses oversize files (src/ai/chat/chatsession.cpp:197-207). Save uses the same maxArchiveBytes.
- Operation read authorization requires schema 2, the live document, and the caller scope (src/workflow/capabilityrecovery.cpp:40-47). Artifact paths go through resolveManagedPath, which rejects ".." segments (src/metadata/managedpath.cpp:19-26, :165-171).
- ChatResultStore read returns result_scope_changed when the reference scope uuid is not the current session scope (src/workflow/chatresultstore.cpp:241-247). setScope drops the previous session's bytes.
- Generated Python uses the sealed sandbox (bwrap --unshare-all plus seccomp on Linux; LPAC, active-process limit 1, no network capability on Windows). No hole found in that enabled profile. Trusted bwrap --bind / is the separate trusted-template profile, not the generated sandbox.
- Context projection drops whole older turns and rejects a current turn that does not fit; it does not split a tool pair and leave a success stub (src/workflow/aichatcontextprojection.cpp:249-267).

## Not filed
- remotepredictrouter health `payload.contains("ok")` also matches `{"ok":false}`. A 2026-10-27 review deferred tightening it because the MAMCL body contract was unconfirmed; not re-filed as a new defect.
- operation_cancel on an already committed record returns ok true with code cancel_after_commit. The tool description documents that code and says the result is kept.
- operation_results with no ports sets code result_unavailable but leaves publicOperation ok true. The code field is explicit; not a silent publish success.
- SGS finite_cells is the last realization only. Cells solvable from static samples are finite in every realization, so this does not turn an all-nodata mean grid into success.
- Windows device names such as CON.json are not rejected by isSafeSessionId. Session ids are minted as UUIDs; no caller plants those names.
- Python shell, network profiles, online install, and persistent interpreter are the deferred items in TODOS.md. Not filed.
- Replay of an existing operation records host effects as Read (src/workflow/capabilityhost.cpp:175-183). recovery start still checks operationEffects before it runs or commits, so this does not publish under a read-only project.
