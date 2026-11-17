// 层：测试壳
// Explicit supply-side build only. Never imported or executed by the application.
import fs from "node:fs";
import path from "node:path";
import { createRequire } from "node:module";

const [esbuildFile, root, entry, output, lockFile] = process.argv.slice(2);
if (!lockFile) throw new Error("supply build arguments required");
const require = createRequire(import.meta.url);
const esbuild = require(esbuildFile);
const lock = JSON.parse(fs.readFileSync(lockFile, "utf8"));
if (process.versions.node !== lock.nodeVersion || esbuild.version !== lock.build.esbuildVersion) throw new Error("toolchain mismatch");
fs.mkdirSync(path.join(root, "paleo"), { recursive: true });
fs.copyFileSync(entry, path.join(root, "paleo/sidecar.mjs"));
const localModules = Object.keys(lock.build.localModules ?? {});
for (const name of localModules) {
  if (name !== "conversation.mjs") throw new Error("unreviewed local runtime module");
  fs.copyFileSync(path.join(path.dirname(entry), name), path.join(root, "paleo", name));
}
const aliases = Object.fromEntries(["agent", "ai", "schema", "telemetry"].map(name => [
  "@earendil-works/pi-" + (name === "agent" ? "agent-core" : name), path.join(root, "packages", name, "src/index.ts"),
]));
aliases["@earendil-works/pi-schema/formats"] = path.join(root, "packages/schema/src/formats/index.ts");
const result = esbuild.buildSync({ absWorkingDir: root, entryPoints: ["paleo/sidecar.mjs"], outfile: "paleo/output.mjs",
  bundle: true, platform: "node", target: "node22", format: "esm", charset: "utf8", legalComments: "inline",
  tsconfigRaw: { compilerOptions: { target: "ES2024", useDefineForClassFields: true, verbatimModuleSyntax: true } },
  packages: "external", alias: aliases, treeShaking: true, metafile: true, write: false,
  define: { PALEO_PI_SOURCE_COMMIT: JSON.stringify(lock.upstream.commit),
    PALEO_PI_CORE_VERSION: JSON.stringify(lock.coreVersion), PALEO_PI_NODE_VERSION: JSON.stringify(lock.nodeVersion) },
});
const graph = Object.values(result.metafile.outputs);
if (graph.some(file => file.imports.length)) throw new Error("runtime bundle retains external imports");
const retained = Object.fromEntries(graph.flatMap(file => Object.entries(file.inputs))
  .filter(([, value]) => value.bytesInOutput > 0).sort(([a], [b]) => a < b ? -1 : a > b ? 1 : 0));
for (const name of Object.keys(retained)) {
  if (!/^(packages\/(agent|ai|schema|telemetry)\/src\/|paleo\/sidecar\.mjs$)/.test(name) &&
      !localModules.some(module => name === "paleo/" + module)) throw new Error("unregistered runtime input");
  if (/packages\/ai\/src\/(api\/|providers\/|auth\/|compat|env-api-keys|models|images)/.test(name))
    throw new Error("provider/auth/runtime resources retained in bundle: " + name);
}
fs.mkdirSync(output, { recursive: true });
fs.writeFileSync(path.join(output, "sidecar.mjs"), result.outputFiles[0].contents);
fs.writeFileSync(path.join(output, "bundle-graph.json"), JSON.stringify({ schema: "paleo.pi-bundle-graph.v1",
  esbuildVersion: esbuild.version, inputs: retained, externalImports: [] }, null, 2) + "\n");
