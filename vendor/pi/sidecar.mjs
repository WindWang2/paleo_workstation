// Trusted optional Pi entry point; no generated JavaScript, model providers or coding-agent resources.
import { Agent } from "@earendil-works/pi-agent-core";
import { AssistantMessageEventStream } from "@earendil-works/pi-ai";

const protocol = "paleo.pi-agent.v1";
const maximum = 64 * 1024;
const sourceCommit = PALEO_PI_SOURCE_COMMIT;
const coreVersion = PALEO_PI_CORE_VERSION;
const nodeVersion = PALEO_PI_NODE_VERSION;
const decoder = new TextDecoder("utf-8", { fatal: true });
let buffer = Buffer.alloc(0);
let hello;
let closing = false;

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

const agent = new Agent({ streamFn: unavailableStream, toolExecution: "sequential", initialState: { tools: [] } });
function canonical(value) {
  return JSON.stringify(Object.fromEntries(Object.keys(value).sort().map(key => [key, value[key]])));
}
function fatal(code) {
  process.stderr.write(code + "\n");
  process.exit(2);
}
function send(value, done) {
  const body = Buffer.from(canonical(value));
  if (body.length > maximum) fatal("pi_frame_budget");
  const header = Buffer.alloc(4); header.writeUInt32LE(body.length);
  process.stdout.write(Buffer.concat([header, body]), done);
}
function validCounter(value) {
  return typeof value === "string" && /^(0|[1-9][0-9]{0,19})$/.test(value) && BigInt(value) <= 18446744073709551615n;
}
function accept(raw) {
  let value;
  try {
    const text = decoder.decode(raw); value = JSON.parse(text);
    if (!value || typeof value !== "object" || Array.isArray(value) || !Buffer.from(canonical(value)).equals(raw)) throw new Error();
  } catch { fatal("pi_frame_invalid"); }
  if (Object.keys(value).sort().join(",") !== "generation,nonce,protocol,sequence,sessionId,type" ||
      value.protocol !== protocol || !validCounter(value.generation) ||
      typeof value.sessionId !== "string" || !/^[a-zA-Z0-9_-]{1,128}$/.test(value.sessionId) ||
      typeof value.nonce !== "string" || !/^[0-9a-f]{32}$/.test(value.nonce)) fatal("pi_frame_identity");
  if (!hello) {
    if (value.type !== "hello" || value.sequence !== "1" || process.versions.node !== nodeVersion) fatal("pi_handshake_invalid");
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
  if (buffer.length + data.length > maximum + 4) fatal("pi_frame_budget");
  buffer = Buffer.concat([buffer, data]);
  if (buffer.length < 4) return;
  const length = buffer.readUInt32LE();
  if (!length || length > maximum) fatal("pi_frame_budget");
  if (buffer.length < length + 4) return;
  if (buffer.length !== length + 4) fatal("pi_frame_state");
  const raw = buffer.subarray(4); buffer = Buffer.alloc(0); accept(raw);
});
process.stdin.on("end", () => { agent.abort(); process.exit(buffer.length ? 2 : 0); });
process.stdin.on("error", () => process.exit(2));
process.stdout.on("error", () => process.exit(2));
setTimeout(() => { agent.abort(); fatal("pi_handshake_timeout"); }, 5000).unref();
