// paleo_workstation 全项目从零评审（swarm）
// 拓扑：勘察规约/债务 → 确定性源码闸门 ∥ 编译+测试套件 → 34 分片并行评审
//       （每条发现独立复核 + 分片内分诊）→ 跨分片归一定级 → 成稿 + 独立审读

// ===================== 类型 =====================

interface SurveyDigest {
  /** 本项目评审必须遵守的规约：分层契约、设计系统、错误处理、构建测试纪律。 */
  conventions: string;
  /** src 各顶层模块的一句话职责。 */
  moduleNotes: string;
}

interface DebtDigest {
  /** 已跟踪/已知债务，一行一条：「模块：条目（来源）」。 */
  entries: string[];
}

interface InfraDigest {
  /** tests/ 布局、命名规律、ctest 跑法与标签口径。 */
  testNotes: string;
  /** 构建系统、CI 车道、tools/ 检查器职责。 */
  buildNotes: string;
}

interface RawFinding {
  /** 相对路径带行号，如 "src/io/lasparser.cpp:123"。 */
  where: string;
  /** 一句话：哪里不对（不是怎么修）。 */
  problem: string;
  /** 证据：实际读到的代码行，或命令与输出。 */
  evidence: string;
  /** high=数据丢失/崩溃/结果错误；medium=明确缺陷但影响有限；low=小的健壮性/风格问题。 */
  severity: "high" | "medium" | "low";
  /** bug | qt-lifecycle | io-robustness | algorithm-correctness | architecture | ux-design | build-ci | performance | test-gap | security | documentation */
  category: string;
}

interface ReviewResult {
  findings: RawFinding[];
}

interface ConfirmResult {
  /** 发现描述是否与代码事实相符（不是是否值得修）。 */
  holds: boolean;
  /** 一句话核实依据。 */
  note: string;
}

interface AreaFinding {
  where: string;
  problem: string;
  evidence: string;
  severity: "high" | "medium" | "low";
  category: string;
  /** 所属分片。 */
  area: string;
  /** verified=独立复核确认；unconfirmed=复核未确认。 */
  status: "verified" | "unconfirmed";
  /** 复核员的一句话依据。 */
  confirmNote: string;
}

interface TriageDecision {
  /** 对应输入清单的序号（0 起）。 */
  index: number;
  /** 是否保留为值得开 issue 的发现。 */
  keep: boolean;
  /** 归一后的严重度。 */
  severity: "high" | "medium" | "low";
  /** 建议 issue 标题（中文，说问题不说位置，≤40字）。 */
  issueTitle: string;
  /** 建议标签 1-2 个。 */
  labels: string[];
  /** 保留/丢弃的一句话理由。 */
  reason: string;
}

interface TriageBatch {
  decisions: TriageDecision[];
}

interface KeptFinding {
  where: string;
  problem: string;
  evidence: string;
  severity: "high" | "medium" | "low";
  category: string;
  area: string;
  status: "verified" | "unconfirmed";
  confirmNote: string;
  issueTitle: string;
  labels: string[];
}

interface KeptFindingList {
  findings: KeptFinding[];
}

interface AreaOutcome {
  area: string;
  kept: KeptFinding[];
  /** 分诊丢弃原因，一行一条。 */
  dropped: string[];
  /** 评审失败原因（空串=成功）。 */
  error: string;
}

interface SynthItem {
  index: number;
  area: string;
  where: string;
  problem: string;
  severity: "high" | "medium" | "low";
  category: string;
  issueTitle: string;
  status: string;
}

interface Synthesis {
  decisions: TriageDecision[];
  /** 最值得优先处理的 5 条，格式 "where：一句话理由"。 */
  highlights: string[];
}

interface BuildOutcome {
  buildExit: number;
  ctestExit: number;
  failedTests: string[];
  findings: KeptFinding[];
  notes: string[];
}

interface ReportDraft {
  /** 完整 markdown 评审报告全文。 */
  markdown: string;
}

interface ReaderCritique {
  /** 报告中不清楚、缺乏支撑、或用户会追问的点。 */
  issues: string[];
}

interface Shard {
  /** 分片名（也是子代理名片）。 */
  label: string;
  /** 该分片评审员的首要视角。 */
  lens: string;
  /** 分片内的源文件（相对路径）。 */
  files: string[];
  /** 附加背景资料（测试/构建分片用）。 */
  extra: string;
}

interface Finding {
  /** Workspace-relative path, with a line when it applies. */
  where: string;
  /** One sentence: what is wrong, or what was found. */
  what: string;
  /** What showed it. */
  evidence: string;
  /** "verified" when an independent subagent or a deterministic check confirmed it. */
  status: "verified" | "unconfirmed";
  /** How much it matters. */
  severity: "low" | "medium" | "high";
}

interface WorkflowReport {
  /** Two or three sentences answering what the user asked for. */
  conclusion: string;
  findings: Finding[];
  /** What the run checked and how. */
  verified: string[];
  /** What the run did not look at or could not check, and why. */
  notCovered: string[];
}

// ===================== 辅助函数 =====================

async function sourcesOf(dirs: string[]): Promise<string[]> {
  const out: string[] = [];
  for (const d of dirs) {
    const all = await files.glob(`${d}/**`);
    for (const p of all) {
      if (p.endsWith(".cpp") || p.endsWith(".h")) out.push(p);
    }
  }
  return out.sort();
}

function chunkFiles(files: string[], max: number): string[][] {
  if (files.length <= max) return [files];
  const mid = Math.ceil(files.length / 2);
  return [...chunkFiles(files.slice(0, mid), max), ...chunkFiles(files.slice(mid), max)];
}

function failedTestNames(output: string): string[] {
  const names: string[] = [];
  for (const line of output.split("\n")) {
    const failedMark =
      line.indexOf("***Failed") >= 0 || line.indexOf("***Not Run") >= 0 || line.indexOf("***Timeout") >= 0;
    if (!failedMark) continue;
    const m = /Test\s+#\d+:\s+(\S+)/.exec(line);
    if (m !== null && m[1] && names.indexOf(m[1]) < 0) names.push(m[1]);
  }
  const summary = /The following tests FAILED:([\s\S]*)$/.exec(output);
  if (summary !== null && summary[1]) {
    for (const line of summary[1].split("\n")) {
      const m = /^\s*\d+\s+-\s+(\S+)/.exec(line);
      if (m !== null && m[1] && names.indexOf(m[1]) < 0) names.push(m[1]);
    }
  }
  return names;
}

function reviewerAsk(shard: Shard, conventions: string, moduleNotes: string, debtEntries: string[]): string {
  const debtText = debtEntries.length > 0 ? debtEntries.join("\n") : "（无登记）";
  const parts: string[] = [
    `你是「${shard.label}」分片的代码评审员，参加一个约 31 万行 C++/Qt/QGIS 古地质工作站项目的从零全面评审。同一项目还有其他分片在并行评审，你只对自己分片内的文件负责。`,
    ``,
    `【项目模块地图】`,
    moduleNotes,
    ``,
    `【评审规约（必须遵守）】`,
    conventions,
    ``,
    `【已知债务（已跟踪事项；除非发现其中没有的新事实，否则不要报告）】`,
    debtText,
  ];
  if (shard.extra !== "") {
    parts.push(``, `【分片背景资料】`, shard.extra);
  }
  parts.push(
    ``,
    `【你的分片】共 ${shard.files.length} 个文件：`,
    shard.files.join("\n"),
    ``,
    `【你的首要视角】${shard.lens}。也欢迎报告你视角之外但确实重要的问题。`,
    ``,
    `【工作要求】`,
    `1. 只读代码：禁止修改、创建或删除任何文件。`,
    `2. 这是从零评审：以代码现状为准，不要轻信注释、文档或既有评审结论的声称。`,
    `3. 每个发现必须带 path:line 和实际读到的代码行作为证据；没读过的代码不准报告。`,
    `4. 严重度口径：high=数据丢失/崩溃/结果错误；medium=明确缺陷但影响有限或有规避路径；low=小的健壮性/一致性/风格问题。`,
    `5. 最多 6 个发现，按严重度从高到低；宁可少报，不要凑数。`,
    `6. 报告前先 grep TODOS.md / AUDIT_ISSUES.md 里与本模块相关的条目，避免重复报告已跟踪事项。`,
    `7. 行号必须准确；引用证据保留原始代码文本。`,
    `8. 读不完那么多文件时，按文件名/规模挑最可能藏问题的优先读，并在发现里如实体现你读过的范围。`,
  );
  return parts.join("\n");
}

function triageAsk(area: string, findings: AreaFinding[], openIssues: string, debtEntries: string[]): string {
  const debtText = debtEntries.length > 0 ? debtEntries.join("\n") : "（无登记）";
  return [
    `分诊员。你在为整个项目从零评审做去重、定级和出 issue 建议。代码事实已由独立复核员核实，你不要重新核实代码。`,
    ``,
    `【本仓库未关闭 issue（JSON）】`,
    openIssues,
    ``,
    `【已知债务（已跟踪，无新信息则丢弃）】`,
    debtText,
    ``,
    `【「${area}」分片已复核的发现（JSON 数组，index 为稳定序号）】`,
    JSON.stringify(findings.map((f, i) => ({ index: i, ...f }))),
    ``,
    `请对每一条给出决策（按 index 回应，漏掉的 index 默认保留）：`,
    `- keep=false 当且仅当：与未关闭 issue 实质重复、或属于已知债务且无新信息、或复核员已说明代码不支持；`,
    `- keep=true 时给 severity（可微调，口径：high=数据丢失/崩溃/结果错误）、issueTitle（中文≤40字，说问题不说位置，去掉分片前缀）、labels（1-2 个，从 bug/qt-lifecycle/io-robustness/algorithm-correctness/architecture/ux-design/build-ci/performance/test-gap/documentation 选）、reason（一句话）。`,
    `只读不写。若拿不准，保留并在 reason 里说明。`,
  ].join("\n");
}

function synthAsk(items: SynthItem[], openIssues: string): string {
  return [
    `归一定级员。下面是整个项目从零评审、经分片分诊后的全部保留发现（每条都已独立复核）。请做最后一道跨分片归一。`,
    ``,
    `【本仓库未关闭 issue（JSON，用于最终去重）】`,
    openIssues,
    ``,
    `【发现清单（JSON 数组，index 为稳定序号）】`,
    JSON.stringify(items),
    ``,
    `请对每一条给出决策（按 index 回应，漏掉的 index 默认保留）：`,
    `- keep=false 当且仅当：与未关闭 issue 实质重复，或明显不是问题；`,
    `- keep=true 时给 severity（跨分片归一：high=数据丢失/崩溃/结果错误；medium=明确缺陷影响有限；low=小问题）、issueTitle（中文≤40字，说问题不说位置，去掉分片前缀）、labels（1-2 个）、reason（一句话）。`,
    `另外给 highlights：最值得优先处理的 5 条，格式 "where：一句话理由"（按优先级排序）。`,
    `只读不写。拿不准就保留。`,
  ].join("\n");
}

function writerAsk(master: KeptFinding[], highlights: string[], verified: string[], notCovered: string[]): string {
  const items = master
    .map(
      (k, i) =>
        [
          `${i + 1}. [${k.severity}][${k.category}] ${k.where}（分片：${k.area}；复核：${k.status}）`,
          `   问题：${k.problem}`,
          `   证据：${k.evidence}`,
          `   复核意见：${k.confirmNote}`,
          `   建议标题：${k.issueTitle}｜建议标签：${k.labels.join("、")}`,
        ].join("\n"),
    )
    .join("\n");
  return [
    `撰稿员。请把下面这份全项目代码评审的最终发现清单写成中文 markdown 报告。`,
    ``,
    `【最值得优先关注的发现】`,
    highlights.join("\n"),
    ``,
    `【全部发现（已逐条独立复核并跨分片归一）】`,
    items,
    ``,
    `【本 run 实际验证过什么】`,
    verified.join("\n"),
    ``,
    `【未覆盖与局限】`,
    notCovered.join("\n"),
    ``,
    `报告要求：`,
    `- 标题：「paleo_workstation 全项目代码评审报告（从零评审）」；`,
    `- 开头 2-3 句结论：总体质量判断 + 最值得动手的点；`,
    `- 按严重度分组的明细，每条含：位置（path:line）、问题、证据、复核状态、建议标签；`,
    `- 低危条目过多时可按主题合并陈述，但不得丢失任何一条的位置与问题描述；`,
    `- 末尾两节：验证范围、未覆盖事项；`,
    `- 只依据给定材料写，不要补充没给出的发现，不要编造数字。`,
    `把完整 markdown 全文放进 markdown 字段返回；不要写任何文件。`,
  ].join("\n");
}

// ===================== 阶段一：勘察 =====================

artifact.table("findings", {
  title: "评审发现（逐条落表）",
  columns: [
    { field: "area", label: "分片" },
    { field: "severity", label: "严重度" },
    { field: "where", label: "位置" },
    { field: "problem", label: "问题" },
    { field: "status", label: "核实" },
  ],
  key: "where",
});

phase("勘察仓库规约、既有债务与测试基建");

const openIssuesRun = await world.run("gh", ["issue", "list", "--state", "open", "--limit", "100", "--json", "number,title"]);
const allIssuesRun = await world.run("gh", [
  "issue",
  "list",
  "--state",
  "all",
  "--limit",
  "400",
  "--json",
  "number,title,state",
]);
const openIssuesJson = openIssuesRun.exitCode === 0 ? openIssuesRun.stdout : "[]";
const allIssuesJson = allIssuesRun.exitCode === 0 ? allIssuesRun.stdout.slice(0, 60000) : "[]";

const [survey, debt, infra] = await Promise.all([
  agent("规约勘察员").ask<SurveyDigest>(
    [
      `请阅读 AGENTS.md、docs/UI_LAYER_PLAN.md、DESIGN.md、README.md、BUILDING.md（长文件用 grep 定位关键段，不必逐字通读），提炼本项目代码评审必须遵守的规约。`,
      `输出 conventions（≤1200字，中文）：分层契约（各层目录与禁令）、设计系统要点（字体/颜色/间距/令牌来源）、错误处理约定、构建与测试纪律（并行度上限、ctest 标签、基线只缩不涨规则）。`,
      `输出 moduleNotes（≤600字）：src 下每个顶层模块（ai/algorithms/app/catalog/domain/io/linkage/metadata/qgis/selfcheck/services/ui/workflow）一句话职责。`,
      `只读不写；以文件现状为准。`,
    ].join("\n"),
  ),
  agent("债务勘察员").ask<DebtDigest>(
    [
      `以下是本仓库全部 GitHub issue（JSON，含 state）：`,
      allIssuesJson,
      ``,
      `另请阅读 TODOS.md 与 AUDIT_ISSUES.md（文件很大，用 grep 按模块/关键词定位小节，不要逐字通读）。`,
      `输出 entries：与 src 代码相关的已跟踪/已知债务/待办，一行一条，格式「模块：条目（来源：issue #N 或 TODOS/AUDIT 章节名）」，≤45 条。`,
      `只读不写；以文件现状为准。`,
    ].join("\n"),
  ),
  agent("测试与构建勘察员").ask<InfraDigest>(
    [
      `勘察本仓库测试与构建基建：tests/ 目录布局与命名规律（用 find/ls，勿逐字读测试体）、CMakeLists.txt 里 add_paleo_test 的接线与 LABELS、.github/workflows/ci.yml 的车道与步骤、tools/ 下各检查器脚本的职责（ls + 读文件头注释）。`,
      `输出 testNotes（≤500字）：测试套件规模、组织方式、ctest 跑法与标签口径、已知薄弱点。`,
      `输出 buildNotes（≤500字）：构建系统、主要 target、CI 车道、源码闸门清单与基线规则。`,
      `只读不写；以文件现状为准。`,
    ].join("\n"),
  ),
]);

log(`勘察完成：规约 ${survey.conventions.length} 字、债务条目 ${debt.entries.length} 条、未关闭 issue ${openIssuesRun.exitCode === 0 ? "已获取" : "获取失败"}`);

// ===================== 并发阶段：编译 + 测试套件 =====================

const buildStage = future(async (): Promise<BuildOutcome> => {
  phase("编译全树并运行测试套件");
  const notes: string[] = [];
  let buildExit = -1;
  let buildTail = "";
  try {
    const build = await world.run("cmake", ["--build", "build", "-j8"], { timeoutMs: 1800000 });
    buildExit = build.exitCode;
    buildTail = (build.stdout + "\n" + build.stderr).slice(-3000);
  } catch (e) {
    notes.push("构建超时或执行失败：" + String(e).slice(0, 300));
  }
  let ctestExit = -1;
  let failed: string[] = [];
  let detail = "";
  try {
    const plain = await world.run("ctest", ["--test-dir", "build", "-j8", "-LE", "core|perf"], {
      timeoutMs: 2700000,
    });
    ctestExit = plain.exitCode;
    failed = failedTestNames(plain.stdout);
    if (failed.length > 0 && failed.length <= 80) {
      try {
        const rerun = await world.run(
          "ctest",
          ["--test-dir", "build", "-j8", "-R", failed.join("|"), "--output-on-failure"],
          { timeoutMs: 900000 },
        );
        detail = (rerun.stdout + "\n" + rerun.stderr).slice(-12000);
      } catch (e2) {
        notes.push("失败详情重跑超时或超限：" + String(e2).slice(0, 200));
      }
    }
    if (failed.length > 80) notes.push(`失败测试 ${failed.length} 项过多，未重跑详情`);
  } catch (e) {
    notes.push("ctest 执行失败或超时：" + String(e).slice(0, 300));
  }
  let findings: KeptFinding[] = [];
  if (buildExit !== 0 || ctestExit !== 0) {
    try {
      const analyzed = await agent("测试失败研判员").ask<KeptFindingList>(
        [
          `测试失败研判员。本地全树编译与测试套件（ctest -j8 -LE core|perf）结果如下，请逐项判断是真代码缺陷还是环境噪音，并转化为 finding。`,
          ``,
          `构建退出码：${buildExit}`,
          `构建输出尾部：`,
          buildTail,
          ``,
          `ctest 退出码：${ctestExit}；失败测试 ${failed.length} 项：${failed.join(", ")}`,
          `失败详情（截断）：`,
          detail,
          ``,
          `要求：`,
          `- 闸门类失败（layering*/qgis_spread*/ui_size*/ui_tokens*/file_budget*/docs_drift 等检查器项）已由另一路研判覆盖，你只需略过，不要重复出 finding；`,
          `- 构建/链接失败、断言失败、段错误等代码缺陷 → finding（where 指向测试或源码，evidence 附输出关键行，status=verified，confirmNote 注明来自构建/测试退出码）；`,
          `- 环境因素（vendored 依赖 ABI 不匹配、缺 QGIS_PREFIX 等）→ 也记为 finding，但 severity 至多 medium、category=build-ci，并在 problem 里注明是本地环境问题；`,
          `- 每条给 issueTitle（中文≤40字）与 labels（1-2 个）；`,
          `- 最多 15 条，按严重度排序；可打开测试与源码阅读确认。只读不写。`,
        ].join("\n"),
      );
      findings = analyzed.findings;
    } catch (e) {
      notes.push("测试失败研判会话失败：" + String(e).slice(0, 300));
    }
  }
  return { buildExit, ctestExit, failedTests: failed, findings, notes };
});

const buildStageSafe: Promise<BuildOutcome> = buildStage.catch((e) => ({
  buildExit: -1,
  ctestExit: -1,
  failedTests: [],
  findings: [],
  notes: ["构建/测试阶段整体失败：" + String(e).slice(0, 300)],
}));

// ===================== 阶段二：确定性源码闸门 =====================

phase("运行确定性源码闸门并研判失败项");

const gateCmds: Array<{ name: string; args: string[] }> = [
  { name: "layering --strict", args: ["tools/check_layering.py", "--strict"] },
  { name: "layering --symbol-audit", args: ["tools/check_layering.py", "--symbol-audit"] },
  { name: "layering --transitive", args: ["tools/check_layering.py", "--transitive"] },
  { name: "layering --qgs-audit --strict", args: ["tools/check_layering.py", "--qgs-audit", "--strict"] },
  { name: "qgis-spread --strict", args: ["tools/check_qgis_spread.py", "--strict"] },
  { name: "ui-invariants --strict", args: ["tools/check_ui_invariants.py", "--strict"] },
  { name: "ui-size --strict", args: ["tools/check_ui_size.py", "--strict"] },
  { name: "file-budget --strict", args: ["tools/check_file_budget.py", "--strict"] },
  { name: "i18n", args: ["tools/check_i18n.py"] },
  { name: "qgis-versions", args: ["tools/check_qgis_versions.py"] },
  { name: "ci-scripts", args: ["tools/check_ci_scripts.py"] },
  { name: "docs-drift", args: ["tools/check_docs_drift.py"] },
  { name: "l10n", args: ["tools/check_l10n.py"] },
  { name: "tidy", args: ["tools/check_tidy.py"] },
];

const gateRuns = await Promise.all(
  gateCmds.map((g) => world.run("python3", g.args, { timeoutMs: 300000 })),
);

const gateFailed: Array<{ name: string; tail: string }> = [];
for (let i = 0; i < gateCmds.length; i++) {
  const r = gateRuns[i];
  if (r.exitCode !== 0) {
    gateFailed.push({ name: gateCmds[i].name, tail: (r.stdout + "\n" + r.stderr).slice(-2500) });
  }
}
log(`源码闸门完成：${gateCmds.length - gateFailed.length} 项通过，${gateFailed.length} 项失败`);

let gateFindings: KeptFinding[] = [];
if (gateFailed.length > 0) {
  try {
    const analyzed = await agent("闸门研判员").ask<KeptFindingList>(
      [
        `闸门研判员。本仓库源码闸门（镜像 CI Source gates 步）有以下失败（退出码非零）：`,
        ...gateFailed.map((g) => `── ${g.name} ──\n${g.tail}`),
        ``,
        `请把每个失败转化为具体 finding：`,
        `- 能定位到文件:行的，给出 where 与证据（可打开文件确认现状后再写）；`,
        `- 闸门自身失败（基线超标/新增散布/文档漂移/静态检查红）也是 finding，where 给最关键的那个文件，problem 说明超标或违规内容；`,
        `- 每条给 severity/category/issueTitle（中文≤40字）/labels（1-2 个）；`,
        `- status 一律 "verified"，confirmNote 注明「闸门 <name> 退出码非零（确定性证据）」；`,
        `- 最多 12 条，按严重度排序。只读不写。`,
      ].join("\n"),
    );
    gateFindings = analyzed.findings;
  } catch (e) {
    log("闸门研判会话失败：" + String(e).slice(0, 200));
  }
}
for (const k of gateFindings) report(k, "findings");

// ===================== 分片定义 =====================

const domainFiles = await sourcesOf(["src/domain"]);
const ioFiles = await sourcesOf(["src/io"]);
const algoAll = await sourcesOf(["src/algorithms"]);
const algoGeo = algoAll.filter(
  (p) => p.startsWith("src/algorithms/geostat/") || p.startsWith("src/algorithms/cluster/") || p.startsWith("src/algorithms/evolution/"),
);
const algoSingle = algoAll.filter((p) => p.startsWith("src/algorithms/singlefactor/"));
const algoRest = algoAll.filter(
  (p) =>
    !p.startsWith("src/algorithms/geostat/") &&
    !p.startsWith("src/algorithms/cluster/") &&
    !p.startsWith("src/algorithms/evolution/") &&
    !p.startsWith("src/algorithms/singlefactor/"),
);
const servicesFiles = await sourcesOf(["src/services"]);
const catalogFiles = await sourcesOf(["src/catalog"]);
const metadataFiles = await sourcesOf(["src/metadata"]);
const workflowFiles = await sourcesOf(["src/workflow"]);
const linkageAiFiles = await sourcesOf(["src/linkage", "src/ai"]);
const qgisFiles = await sourcesOf(["src/qgis"]);
const appFiles = await sourcesOf(["src/app", "src/selfcheck"]);
const uiAll = await sourcesOf(["src/ui"]);
const uiMainwin = uiAll.filter(
  (p) =>
    /^src\/ui\/paleomainwindow/.test(p) ||
    p === "src/ui/paleoribbon.cpp" ||
    p === "src/ui/paleoribbon.h" ||
    p === "src/ui/paleodockmanager.cpp" ||
    p === "src/ui/paleodockmanager.h" ||
    p === "src/ui/paleotheme.cpp" ||
    p === "src/ui/paleotheme.h" ||
    p === "src/ui/uienv_internal.h",
);
const uiPages = uiAll.filter((p) => p.startsWith("src/ui/pages/"));
const uiWellComposite = uiAll.filter((p) => p.startsWith("src/ui/wellcomposite/"));
const uiSeismicSection = uiAll.filter((p) => p.startsWith("src/ui/seismicsection/"));
const uiSeismic3d = uiAll.filter((p) => p.startsWith("src/ui/seismic3d/"));
const uiDataPreview = uiAll.filter((p) => p.startsWith("src/ui/datapreview/"));
const uiWellSection = uiAll.filter((p) => p.startsWith("src/ui/wellsection/") || p.startsWith("src/ui/welltops/"));
const uiMapTools = uiAll.filter(
  (p) => p.startsWith("src/ui/maptools/") || p.startsWith("src/ui/layers/") || p.startsWith("src/ui/correlation/"),
);
const uiNamed = uiAll.filter(
  (p) =>
    p.startsWith("src/ui/pages/") ||
    p.startsWith("src/ui/wellcomposite/") ||
    p.startsWith("src/ui/seismicsection/") ||
    p.startsWith("src/ui/seismic3d/") ||
    p.startsWith("src/ui/datapreview/") ||
    p.startsWith("src/ui/wellsection/") ||
    p.startsWith("src/ui/welltops/") ||
    p.startsWith("src/ui/maptools/") ||
    p.startsWith("src/ui/layers/") ||
    p.startsWith("src/ui/correlation/"),
);
const uiRest = uiAll.filter((p) => uiNamed.indexOf(p) < 0 && uiMainwin.indexOf(p) < 0);
const testsFiles = (await files.glob("tests/**")).filter((p) => p.endsWith(".cpp") || p.endsWith(".h")).sort();

const buildFilesList: string[] = [];
for (const pattern of ["cmake/**", "scripts/**", "tools/**", ".github/**"]) {
  const found = await files.glob(pattern);
  for (const p of found) {
    if (p.indexOf("__pycache__") < 0 && !p.endsWith(".pyc")) buildFilesList.push(p);
  }
}
for (const p of ["CMakeLists.txt", "paleo-dev", "paleo-dev.ps1", "run-target-tests.sh", "BUILDING.md"]) {
  buildFilesList.push(p);
}
buildFilesList.sort();

const shardDefs: Shard[] = [];
function addShard(label: string, lens: string, files: string[], max: number, extra?: string) {
  const parts = chunkFiles(files, max);
  for (let i = 0; i < parts.length; i++) {
    shardDefs.push({
      label: parts.length > 1 ? `${label}（${i + 1}/${parts.length}）` : label,
      lens,
      files: parts[i],
      extra: extra === undefined ? "" : extra,
    });
  }
}

addShard("domain 领域模型", "值语义与不变量：拷贝/共享、写时复制、空值与单位、数值精度、跨模块契约", domainFiles, 80);
addShard("io 文件解析与落盘", "解析健壮性：畸形输入、越界读、整数溢出、编码识别、截断；落盘原子性、临时文件与锁", ioFiles, 80);
addShard("algorithms 地质统计与演化", "数值正确性：除零、NaN 传播、收敛判据、量纲、矩阵条件数", algoGeo, 80);
addShard("algorithms 单因素建模", "数值正确性：迭代稳定性、边界条件、权重归一、采样一致性", algoSingle, 80);
addShard("algorithms 地震与栅格", "DSP 与栅格数值正确性：FFT 前后处理、边界像元、反演收敛", algoRest, 80);
addShard("services 业务服务", "服务契约：事务性、线程安全、错误传播、取消语义", servicesFiles, 80);
addShard("catalog 资产目录", "目录一致性：索引、查询正确性、版本迁移、并发读写", catalogFiles, 80);
addShard("metadata 工程元数据", "持久化健壮性：损坏恢复、迁移、原子写、schema 演进", metadataFiles, 80);
addShard("workflow 编排", "编排正确性：状态机、错误传播、取消与幂等、批量任务队列", workflowFiles, 80);
addShard("linkage 与 ai 服务", "图算法正确性、模型加载与推理服务健壮性", linkageAiFiles, 80);
addShard("qgis 封装层", "封装正确性：Qgs 类型泄漏、坐标/投影转换、资源所有权", qgisFiles, 80);
addShard("app 组装根", "启动/关闭顺序、单例与依赖装配、selfcheck 完整性", appFiles, 80);
addShard("主窗家族", "Qt 生命周期：父权、Lambda 捕获、跨线程信号槽、attach 域装配", uiMainwin, 80);
addShard("ui/pages 页面族", "Qt 生命周期与性能：长任务、大数据表、设计系统符合度", uiPages, 80);
addShard("ui/wellcomposite 井剖面", "绘制性能与缓存、Qt 生命周期、刷新策略", uiWellComposite, 80);
addShard("ui/seismicsection 地震剖面", "大数据渲染、内存峰值、手势与交互状态", uiSeismicSection, 80);
addShard("ui/seismic3d 三维", "3D 资源管理、线程与渲染上下文", uiSeismic3d, 80);
addShard("ui/datapreview 数据预览", "大文件流式与内存、格式分支正确性", uiDataPreview, 80);
addShard("ui/wellsection 联井剖面", "交互状态机、绘制与数据同步", uiWellSection, 80);
addShard("ui 地图工具与图层", "地图工具状态机、选择同步、Qgs 交互封装", uiMapTools, 80);
addShard("ui 其余组件", "DESIGN.md 设计系统符合度（令牌/字体/间距）、Qt 生命周期", uiRest, 80);
addShard("tests 测试套件", "测试质量：覆盖缺口、墙钟/sleep 断言、跳过与弱断言、与实现脱节", testsFiles, 150, infra.testNotes);
addShard("构建与 CI", "CMake/CI 可移植性、检查器脚本缺陷、测试接线", buildFilesList, 200, infra.buildNotes);

const totalFiles = shardDefs.reduce((acc, s) => acc + s.files.length, 0);
log(`${shardDefs.length} 个分片、${totalFiles} 个源文件进入并行评审`);

// ===================== 阶段三：并行评审 =====================

phase("并行评审全部模块代码");

const areaPromises = shardDefs.map((shard) =>
  (async (): Promise<AreaOutcome> => {
    let review: ReviewResult;
    try {
      review = await agent(`评审员·${shard.label}`, {
        system:
          "你是资深 C++/Qt 代码评审员。只读代码，永不修改文件；每个发现都要有 path:line 证据；" +
          "宁可少报也不报没根据的；遇到读不懂或自相矛盾的地方如实说明，不要猜测。",
      }).ask<ReviewResult>(reviewerAsk(shard, survey.conventions, survey.moduleNotes, debt.entries));
    } catch (e) {
      return { area: shard.label, kept: [], dropped: [], error: "评审会话失败：" + String(e).slice(0, 300) };
    }
    const raw = review.findings.slice(0, 6);
    const confirmed: AreaFinding[] = await Promise.all(
      raw.map(async (f, fi): Promise<AreaFinding> => {
        try {
          const c = await agent(`复核员·${shard.label}-${fi + 1}`).ask<ConfirmResult>(
            [
              `独立复核员。请只依据证据核实下面这条评审发现是否与代码事实相符（不是判断是否值得修）。`,
              ``,
              `发现（JSON）：${JSON.stringify(f)}`,
              ``,
              `要求：打开 where 指明的文件读相关代码（可读周边上下文）；判定问题描述是否属实。`,
              `禁止修改任何文件。若文件不存在、行号漂移或描述含糊无法判定，holds=false 并说明原因。`,
            ].join("\n"),
          );
          return {
            where: f.where,
            problem: f.problem,
            evidence: f.evidence,
            severity: f.severity,
            category: f.category,
            area: shard.label,
            status: c.holds ? "verified" : "unconfirmed",
            confirmNote: c.note,
          };
        } catch (e) {
          return {
            where: f.where,
            problem: f.problem,
            evidence: f.evidence,
            severity: f.severity,
            category: f.category,
            area: shard.label,
            status: "unconfirmed",
            confirmNote: "复核会话失败：" + String(e).slice(0, 200),
          };
        }
      }),
    );
    if (confirmed.length === 0) {
      return { area: shard.label, kept: [], dropped: [], error: "" };
    }
    let kept: KeptFinding[] = [];
    const dropped: string[] = [];
    try {
      const triage = await agent(`分诊员·${shard.label}`).ask<TriageBatch>(
        triageAsk(shard.label, confirmed, openIssuesJson, debt.entries),
      );
      for (const d of triage.decisions) {
        if (d.index < 0 || d.index >= confirmed.length) continue;
        const f = confirmed[d.index];
        if (!d.keep) {
          dropped.push(`${f.where}：${d.reason}`);
          continue;
        }
        kept.push({
          where: f.where,
          problem: f.problem,
          evidence: f.evidence,
          severity: d.severity,
          category: f.category,
          area: shard.label,
          status: f.status,
          confirmNote: f.confirmNote,
          issueTitle: d.issueTitle,
          labels: d.labels,
        });
      }
      for (let i = 0; i < confirmed.length; i++) {
        const decided = triage.decisions.some((d) => d.index === i);
        if (!decided) {
          const f = confirmed[i];
          kept.push({
            where: f.where,
            problem: f.problem,
            evidence: f.evidence,
            severity: f.severity,
            category: f.category,
            area: shard.label,
            status: f.status,
            confirmNote: f.confirmNote,
            issueTitle: f.problem.slice(0, 40),
            labels: [f.category],
          });
        }
      }
    } catch (e) {
      for (const f of confirmed) {
        kept.push({
          where: f.where,
          problem: f.problem,
          evidence: f.evidence,
          severity: f.severity,
          category: f.category,
          area: shard.label,
          status: f.status,
          confirmNote: f.confirmNote,
          issueTitle: f.problem.slice(0, 40),
          labels: [f.category],
        });
      }
      dropped.push("分诊会话失败，原样保留全部：" + String(e).slice(0, 200));
    }
    for (const k of kept) report(k, "findings");
    return { area: shard.label, kept, dropped, error: "" };
  })(),
);

// 屏障：跨分片归一定级需要每个分片的保留发现全部到齐（去重与严重度归一是全局操作）
const outcomes: AreaOutcome[] = await Promise.all(areaPromises);
const buildOutcome: BuildOutcome = await buildStageSafe;
for (const k of buildOutcome.findings) report(k, "findings");

log(`评审完成：${outcomes.length} 个分片，失败 ${outcomes.filter((o) => o.error !== "").length} 个；构建退出码 ${buildOutcome.buildExit}，测试失败 ${buildOutcome.failedTests.length} 项`);

// ===================== 阶段四：跨分片归一 =====================

phase("跨分片归一、定级并形成主清单");

const allKept: KeptFinding[] = [];
for (const o of outcomes) {
  for (const k of o.kept) allKept.push(k);
}
for (const k of buildOutcome.findings) allKept.push(k);
for (const k of gateFindings) allKept.push(k);

const synthItems: SynthItem[] = allKept.map((k, i) => ({
  index: i,
  area: k.area,
  where: k.where,
  problem: k.problem,
  severity: k.severity,
  category: k.category,
  issueTitle: k.issueTitle,
  status: k.status,
}));

let highlights: string[] = [];
const decisionByIndex = new Map<number, TriageDecision>();
if (synthItems.length > 0) {
  try {
    const synth = await agent("归一定级员").ask<Synthesis>(synthAsk(synthItems, openIssuesJson));
    for (const d of synth.decisions) decisionByIndex.set(d.index, d);
    highlights = synth.highlights;
  } catch (e) {
    log("归一会话失败，按分诊结果原样采用：" + String(e).slice(0, 200));
  }
}

const master: KeptFinding[] = [];
for (let i = 0; i < allKept.length; i++) {
  const k = allKept[i];
  const d = decisionByIndex.get(i);
  if (d !== undefined && !d.keep) continue;
  master.push({
    where: k.where,
    problem: k.problem,
    evidence: k.evidence,
    severity: d === undefined ? k.severity : d.severity,
    category: k.category,
    area: k.area,
    status: k.status,
    confirmNote: k.confirmNote,
    issueTitle: d === undefined || d.issueTitle === "" ? k.issueTitle : d.issueTitle,
    labels: d === undefined || d.labels.length === 0 ? k.labels : d.labels,
  });
}

const highCount = master.filter((k) => k.severity === "high").length;
const mediumCount = master.filter((k) => k.severity === "medium").length;
const lowCount = master.filter((k) => k.severity === "low").length;

// ===================== 阶段五：成稿 + 独立审读 =====================

phase("撰写评审报告并独立审读");

const verified: string[] = [];
for (let i = 0; i < gateCmds.length; i++) {
  const r = gateRuns[i];
  verified.push(
    `源码闸门 ${gateCmds[i].name}：退出码 ${r.exitCode}${r.exitCode === 0 ? "（通过）" : "（失败，已转化为发现）"}`,
  );
}
verified.push(`全树编译 cmake --build build -j8：退出码 ${buildOutcome.buildExit}`);
verified.push(
  `测试套件 ctest --test-dir build -j8 -LE core|perf：退出码 ${buildOutcome.ctestExit}，失败 ${buildOutcome.failedTests.length} 项${buildOutcome.failedTests.length > 0 ? `（${buildOutcome.failedTests.slice(0, 15).join(", ")}${buildOutcome.failedTests.length > 15 ? " …" : ""}）` : ""}`,
);
verified.push(
  `代码评审覆盖 ${shardDefs.length} 个分片、${totalFiles} 个源文件；每条发现均由未参与该分片评审的独立复核员核实，状态标注 verified/unconfirmed。`,
);

const notCovered: string[] = [
  "vendor/ 下第三方依赖（gitignored）未评审；",
  "check_layering.py --transitive 的 func/ui 两项指标未跑：本地缺少 QGIS_PREFIX（vendor/prefix/usr 不存在），该指标只在 CI lint 车道全闸；",
  "运行时/真机交互未验证：无 GUI 环境，UI 行为、性能体感与崩溃复现均未实测；",
  "Windows 侧行为与 osgeo4w 钉版本未验证；",
  "docs/、resources/、translations/、prototype/、spikes/、demo/、testdata/ 未纳入代码评审范围（文档漂移由 check_docs_drift 闸门覆盖）；",
];
for (const o of outcomes) {
  if (o.error !== "") notCovered.push(`分片「${o.area}」评审失败：${o.error}`);
}
for (const n of buildOutcome.notes) notCovered.push(`构建/测试阶段备注：${n}`);

let draft: ReportDraft = { markdown: "" };
const writer = agent("撰稿员");
try {
  draft = await writer.ask<ReportDraft>(writerAsk(master, highlights, verified, notCovered));
} catch (e) {
  log("撰稿会话失败：" + String(e).slice(0, 200));
}

let finalMarkdown = draft.markdown;
if (finalMarkdown !== "") {
  try {
    const critique = await agent("审读员").ask<ReaderCritique>(
      [
        `审读员。请只凭文本审读下面这份评审报告（不要读代码、不要核实发现——那是复核员的职责）。`,
        ``,
        finalMarkdown.slice(0, 60000),
        ``,
        `请逐条列出：哪些地方表述不清、哪些结论缺乏报告内支撑、读者下一步最可能追问什么、结构上缺什么。`,
      ].join("\n"),
    );
    if (critique.issues.length > 0) {
      finalMarkdown = (
        await writer.ask<ReportDraft>(
          [
            `撰稿员。请按审读员意见修订你的报告初稿（初稿在你上一步上下文里，此处再附一份）。`,
            ``,
            `初稿：`,
            draft.markdown.slice(0, 60000),
            ``,
            `审读意见：`,
            critique.issues.join("\n"),
            ``,
            `逐条回应修订；不得删减任何发现条目。把修订后的完整 markdown 全文放进 markdown 字段返回；不要写文件。`,
          ].join("\n"),
        )
      ).markdown;
    }
  } catch (e) {
    log("独立审读会话失败，采用初稿：" + String(e).slice(0, 200));
  }
}

if (finalMarkdown !== "") {
  try {
    await artifact.markdown("review-report", finalMarkdown, {
      title: "全项目代码评审报告（从零评审）",
      description: `${shardDefs.length} 个分片并行评审、逐条独立复核后的发现清单，共 ${master.length} 条。`,
      primary: true,
    });
  } catch (e) {
    log("报告发布失败：" + String(e).slice(0, 200));
  }
}

const topHighlight = highlights.length > 0 ? highlights[0] : "见报告明细";

return {
  conclusion: `从零全项目评审完成：${shardDefs.length} 个分片覆盖 ${totalFiles} 个源文件，逐条独立复核并跨分片归一后保留 ${master.length} 条发现（高危 ${highCount}、中危 ${mediumCount}、低危 ${lowCount}）。确定性检查方面：源码闸门 ${gateFailed.length} 项失败、全树编译退出码 ${buildOutcome.buildExit}、测试套件失败 ${buildOutcome.failedTests.length} 项，均已转化为发现或记入未覆盖。最优先处理：${topHighlight}`,
  findings: master.map((k) => ({
    where: k.where,
    what: k.problem,
    evidence: `[${k.area}] ${k.evidence.slice(0, 400)}｜复核(${k.status})：${k.confirmNote.slice(0, 200)}｜建议：${k.issueTitle}｜${k.labels.join("/")}`,
    status: k.status,
    severity: k.severity,
  })),
  verified,
  notCovered,
};
