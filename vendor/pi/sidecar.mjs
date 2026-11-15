// Trusted optional Pi entry point; no generated JavaScript, model providers or coding-agent resources.
import { Agent } from "@earendil-works/pi-agent-core";
import { AssistantMessageEventStream } from "@earendil-works/pi-ai";
import { createConversation } from "./conversation.mjs";

const protocol = "paleo.pi-agent.v1";
const maximum = 64 * 1024;
const sourceCommit = PALEO_PI_SOURCE_COMMIT;
const coreVersion = PALEO_PI_CORE_VERSION;
const nodeVersion = PALEO_PI_NODE_VERSION;
const decoder = new TextDecoder("utf-8", { fatal: true });
let buffer = Buffer.alloc(0);
let hello;
let closing = false;
let conversation, selectedProtocol;
let pendingBytes = 0;

function unavailableStream() {
  const stream = new AssistantMessageEventStream();
  stream.push({ type: "error", reason: "error", error: {
    role: "assistant", content: [], api: "paleo-host", provider: "paleo-host", model: "unconnected",
    usage: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, totalTokens: 0,
      cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, total: 0 } },
    stopReason: "error", errorMessage: "pi_model_transport_unconnected", timestamp: Date.now(),
  } });
  stream.end();
  return stream;
}

let agent;
function canonical(value) {
  const ordered = item => Array.isArray(item) ? item.map(ordered) : item && typeof item === "object" ?
    Object.fromEntries(Object.keys(item).sort().map(key => [key, ordered(item[key])])) : item;
  return JSON.stringify(ordered(value));
}
function fatal(code) {
  process.stderr.write(code + "\n");
  process.exit(2);
}
function send(value, done) {
  const body = Buffer.from(canonical(value));
  if (body.length > maximum) fatal("pi_frame_budget");
  const header = Buffer.alloc(4); header.writeUInt32LE(body.length);
  const frame = Buffer.concat([header, body]);
  if (pendingBytes + frame.length > 256 * 1024) fatal("pi_outgoing_budget");
  pendingBytes += frame.length;
  return new Promise((resolve, reject) => process.stdout.write(frame, error => {
    pendingBytes -= frame.length;
    if (error) { reject(error); return; }
    if (done) done(); resolve();
  }));
}
function validCounter(value) {
  return matches(/^(0|[1-9][0-9]{0,19})$/, value) && BigInt(value) <= 18446744073709551615n;
}
function matches(pattern, value) { return typeof value === "string" && pattern.exec(value)?.[0] === value; }
function accept(raw) {
  let value;
  try {
    const text = decoder.decode(raw); value = JSON.parse(text);
    if (!value || typeof value !== "object" || Array.isArray(value) || !Buffer.from(canonical(value)).equals(raw)) throw new Error();
  } catch { fatal("pi_frame_invalid"); }
  if (!selectedProtocol) {
    selectedProtocol = value.protocol;
    if (selectedProtocol === "paleo.pi-conversation.v1") {
      clearTimeout(watchdog);
      setTimeout(() => fatal("pi_turn_timeout"), 30 * 60 * 1000).unref();
      conversation = createConversation({ Agent, AssistantMessageEventStream, send, fatal,
        versions: { upstreamCommit: sourceCommit, coreVersion, nodeVersion } });
    }
  }
  if (conversation) { conversation.accept(value); return; }
  if (Object.keys(value).sort().join(",") !== "generation,nonce,protocol,sequence,sessionId,type" ||
      value.protocol !== protocol || !validCounter(value.generation) ||
      !matches(/^[a-zA-Z0-9_-]{1,128}$/, value.sessionId) || !matches(/^[0-9a-f]{32}$/, value.nonce)) fatal("pi_frame_identity");
  if (!hello) {
    if (value.type !== "hello" || value.sequence !== "1" || process.versions.node !== nodeVersion) fatal("pi_handshake_invalid");
    agent = new Agent({ streamFn: unavailableStream, toolExecution: "sequential", initialState: { tools: [] } });
    hello = value;
    send({ ...value, type: "hello_result", upstreamCommit: sourceCommit, coreVersion, nodeVersion,
      toolExecution: agent.toolExecution, modelTransport: "host-only-unconnected", sandboxed: false });
  } else {
    if (value.type !== "close" || value.sequence !== "2" || value.sessionId !== hello.sessionId ||
        value.generation !== hello.generation || value.nonce !== hello.nonce) fatal("pi_frame_state");
    closing = true; agent.abort();
    void agent.waitForIdle().then(() => send({ ...value, type: "closed" }, () => process.exit(0)));
  }
}
process.stdin.on("data", data => {
  if (closing) fatal("pi_frame_state");
  let offset = 0;
  while (offset < data.length) {
    const count = Math.min(maximum + 4 - buffer.length, data.length - offset);
    if (count <= 0) fatal("pi_frame_budget");
    buffer = Buffer.concat([buffer, data.subarray(offset, offset + count)]); offset += count;
    while (buffer.length >= 4) {
      const length = buffer.readUInt32LE();
      if (!length || length > maximum) fatal("pi_frame_budget");
      if (buffer.length < length + 4) break;
      const raw = buffer.subarray(4, length + 4); buffer = buffer.subarray(length + 4);
      accept(raw);
      if (!conversation && (buffer.length || offset < data.length)) fatal("pi_frame_state");
    }
  }
});
process.stdin.on("end", () => { agent?.abort(); conversation?.end(); process.exit(buffer.length ? 2 : 0); });
process.stdin.on("error", () => process.exit(2));
process.stdout.on("error", () => process.exit(2));
const watchdog = setTimeout(() => { agent?.abort(); fatal("pi_handshake_timeout"); }, 5000).unref();
