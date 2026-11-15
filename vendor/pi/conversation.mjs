// Fixed reviewed A2 orchestration. All model transport/tool effects stay in C++.
export function createConversation({ Agent, AssistantMessageEventStream, send, fatal, versions }) {
  const protocol = "paleo.pi-conversation.v1";
  let identity, hostSequence = 0n, peerSequence = 0n, phase = "opening";
  let agent, model, round = 0, definitionsBytes = 0, abortReason = "", totalTextBytes = 0, modelFailed = false;
  const tools = new Map(), calls = new Map(), results = new Map(), pending = new Map();
  const zeroUsage = () => ({ input: 0, output: 0, cacheRead: 0, cacheWrite: 0, totalTokens: 0,
    cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, total: 0 } });
  const keys = (value, names) => value && typeof value === "object" && !Array.isArray(value) &&
    Object.keys(value).sort().join(",") === names.split(",").sort().join(",");
  const safeId = value => typeof value === "string" && /^[a-zA-Z0-9_-]{1,128}$/.test(value);
  const counter = value => typeof value === "string" && /^(0|[1-9][0-9]{0,19})$/.test(value) && BigInt(value) <= 18446744073709551615n;
  const emit = (type, payload = {}) => send({ ...identity, protocol, type, payload, sequence: String(++peerSequence) });
  const partial = value => ({ ...value, content: value.content.map(item => ({ ...item })) });
  function failModel(code) {
    if (!model) return;
    const current = model; model = undefined;
    if (!abortReason) modelFailed = true;
    const reason = abortReason ? "aborted" : "error";
    current.message.stopReason = reason; current.message.errorMessage = code;
    current.stream.push({ type: "error", reason, error: partial(current.message) }); current.stream.end();
  }
  function toolError(code) { return { content: [{ type: "text", text: JSON.stringify({ code }) }], isError: true }; }
  function settle(code) {
    failModel(code);
    for (const entry of pending.values()) entry.resolve(toolError(code)); pending.clear();
  }
  function streamFn(_model, _context, options) {
    const stream = new AssistantMessageEventStream();
    const message = { role: "assistant", content: [{ type: "text", text: "" }], api: "paleo-host", provider: "paleo-host",
      model: "host-owned", usage: zeroUsage(), stopReason: "pending", timestamp: Date.now() };
    if (model || phase !== "running" || ++round > 6) { fatal("pi_model_state"); return stream; }
    model = { stream, message, round: String(round), bytes: 0, ids: new Set() };
    stream.push({ type: "start", partial: partial(message) });
    if (options?.signal) options.signal.addEventListener("abort", () => failModel(abortReason || "interrupted"), { once: true });
    void emit("model_request", { round: String(round) }).catch(() => fatal("pi_process_write_failed"));
    return stream;
  }
  async function run() {
    agent = new Agent({ streamFn, toolExecution: "sequential", initialState: { tools: [...tools.values()] } });
    // This awaited event also covers core schema/unknown-tool rejections. C++
    // validates/returns every admitted call, so model history never lacks a result.
    agent.subscribe(async (event, signal) => {
      if (event.type !== "tool_execution_start" || signal.aborted) return;
      const call = calls.get(event.toolCallId);
      if (!call || pending.has(call.id) || results.has(call.id)) fatal("pi_tool_state");
      const result = new Promise(resolve => pending.set(call.id, { resolve, round: call.round }));
      await emit("tool_request", { round: call.round, callId: call.id });
      results.set(call.id, await result);
    });
    try {
      await agent.prompt("Host-owned conversation turn; model context and authorization remain in C++.");
      await agent.waitForIdle();
      if (pending.size || model) fatal("pi_idle_state");
      phase = "idle"; await emit("idle", { code: abortReason || (modelFailed ? "error" : "ok") });
    } catch { fatal("pi_agent_failed"); }
  }
  function abort(reason) {
    if (reason !== "cancelled" && reason !== "interrupted") fatal("pi_abort_invalid");
    abortReason = reason; agent?.abort(); settle(reason);
    if (!agent) { phase = "idle"; void emit("idle", { code: reason }).catch(() => fatal("pi_process_write_failed")); }
  }
  function modelFrame(type, payload) {
    if (!model || payload.round !== model.round || abortReason) fatal("pi_model_state");
    const current = model;
    if (type === "model_delta") {
      if (!keys(payload, "round,text") || typeof payload.text !== "string") fatal("pi_model_invalid");
      const bytes = Buffer.byteLength(payload.text);
      current.bytes += bytes; totalTextBytes += bytes;
      if (bytes > 16 * 1024 || current.bytes > 4 * 1024 * 1024 || totalTextBytes > 24 * 1024 * 1024) fatal("pi_model_budget");
      current.message.content[0].text += payload.text;
      current.stream.push({ type: "text_delta", contentIndex: 0, delta: payload.text, partial: partial(current.message) });
    } else if (type === "model_call") {
      if (!keys(payload, "round,callId,name,argumentsJson") || !safeId(payload.callId) || typeof payload.name !== "string" ||
          payload.name.length > 128 || typeof payload.argumentsJson !== "string" || Buffer.byteLength(payload.argumentsJson) > 16 * 1024 ||
          current.ids.has(payload.callId) || calls.has(payload.callId) || current.ids.size >= 8 || calls.size >= 16) fatal("pi_tool_invalid");
      let argumentsValue;
      try { argumentsValue = JSON.parse(payload.argumentsJson); } catch { fatal("pi_tool_invalid"); }
      if (!argumentsValue || typeof argumentsValue !== "object" || Array.isArray(argumentsValue)) fatal("pi_tool_invalid");
      current.ids.add(payload.callId); calls.set(payload.callId, { id: payload.callId, round: current.round });
      const call = { type: "toolCall", id: payload.callId, name: payload.name, arguments: argumentsValue };
      current.message.content.push(call);
      current.stream.push({ type: "toolcall_end", contentIndex: current.message.content.length - 1, toolCall: call, partial: partial(current.message) });
    } else if (type === "model_end") {
      if (!keys(payload, "round,reason") || typeof payload.reason !== "string") fatal("pi_model_invalid");
      current.message.stopReason = current.ids.size ? "toolUse" : payload.reason === "length" ? "length" : "stop";
      model = undefined; current.stream.push({ type: "done", reason: current.message.stopReason, message: partial(current.message) }); current.stream.end();
    } else {
      if (!keys(payload, "round,code") || typeof payload.code !== "string" || !/^[a-zA-Z0-9_-]{1,128}$/.test(payload.code)) fatal("pi_model_invalid");
      failModel(payload.code);
    }
  }
  function accept(frame) {
    if (!keys(frame, "protocol,type,sessionId,generation,turnId,nonce,sequence,payload") || frame.protocol !== protocol ||
        !counter(frame.sequence) || BigInt(frame.sequence) !== ++hostSequence || !keys(frame.payload, Object.keys(frame.payload ?? {}).join(","))) fatal("pi_conversation_identity");
    if (!identity) {
      if (process.versions.node !== versions.nodeVersion) fatal("pi_node_version");
      if (frame.type !== "open" || !keys(frame.payload, "") || !safeId(frame.sessionId) || !safeId(frame.turnId) ||
          !counter(frame.generation) || typeof frame.nonce !== "string" || !/^[0-9a-f]{32}$/.test(frame.nonce)) fatal("pi_conversation_identity");
      identity = { sessionId: frame.sessionId, generation: frame.generation, turnId: frame.turnId, nonce: frame.nonce };
      phase = "configuring";
      void emit("opened", { ...versions, toolExecution: "sequential", modelTransport: "host-only", sandboxed: false }).catch(() => fatal("pi_process_write_failed"));
      return;
    }
    for (const key of ["sessionId", "generation", "turnId", "nonce"]) if (frame[key] !== identity[key]) fatal("pi_conversation_identity");
    if (frame.type === "abort") {
      if (!keys(frame.payload, "reason") || phase === "closed") fatal("pi_abort_invalid"); abort(frame.payload.reason); return;
    }
    if (frame.type === "close") {
      if (!keys(frame.payload, "") || phase !== "idle") fatal("pi_close_state"); phase = "closed";
      void emit("closed").then(() => process.exit(0), () => fatal("pi_process_write_failed")); return;
    }
    if (frame.type === "tool_definition") {
      const value = frame.payload;
      if (phase !== "configuring" || !keys(value, "name,description,parametersJson") || typeof value.name !== "string" ||
          !/^paleo\.[a-zA-Z0-9_]+$/.test(value.name) || typeof value.description !== "string" ||
          typeof value.parametersJson !== "string" || tools.has(value.name) || tools.size >= 128) fatal("pi_tool_definition_invalid");
      definitionsBytes += Buffer.byteLength(JSON.stringify(value)); if (definitionsBytes > 512 * 1024) fatal("pi_configuration_budget");
      let parameters; try { parameters = JSON.parse(value.parametersJson); } catch { fatal("pi_tool_definition_invalid"); }
      if (!parameters || typeof parameters !== "object" || Array.isArray(parameters)) fatal("pi_tool_definition_invalid");
      tools.set(value.name, { name: value.name, label: value.name, description: value.description, parameters,
        execute: async callId => { if (!results.has(callId)) fatal("pi_tool_state"); return results.get(callId); } });
      void emit("configured", { name: value.name }).catch(() => fatal("pi_process_write_failed")); return;
    }
    if (frame.type === "start") {
      if (phase !== "configuring" || !keys(frame.payload, "")) fatal("pi_start_state"); phase = "running"; void run(); return;
    }
    if (frame.type === "tool_result") {
      const value = frame.payload, entry = pending.get(value.callId);
      if (!keys(value, "round,callId,ok,content") || !entry || value.round !== entry.round || typeof value.ok !== "boolean" ||
          typeof value.content !== "string" || Buffer.byteLength(value.content) > 32 * 1024) fatal("pi_tool_result_invalid");
      pending.delete(value.callId); entry.resolve({ content: [{ type: "text", text: value.content }], isError: !value.ok }); return;
    }
    if (!["model_delta", "model_call", "model_end", "model_error"].includes(frame.type)) fatal("pi_conversation_type");
    modelFrame(frame.type, frame.payload);
  }
  return { accept, end: () => { agent?.abort(); settle("interrupted"); } };
}
