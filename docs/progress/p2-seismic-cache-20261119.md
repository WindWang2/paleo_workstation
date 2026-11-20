# P2：SEG-Y 索引可信恢复与时间切片缓存一致性（2026-11-19 开工）

- 分支：`codex/paleo-p2-seismic-cache-20261119`（worktree `../paleo-p2-seismic-cache-20261119`）
- 基线：`origin/master = 08ca3e52b829f928d38e34d3f7fa3e58376f6076`（与 prompt 研究基线一致）
- 开放 PR：0；#1024/#1025/#1026 开放中，为本任务目标（已实读）。
- 依赖前缀：主仓 `vendor/superbuild/prefix`（11-04 构建）**不含 #1042-V6 收据戳**，
  master 新依赖闸拒收；按 BUILDING.md「Linux 二进制加速档」在本 worktree 跑
  `./paleo-dev bootstrap fetch-only`（deb 闭包，glibc 2.44≥2.43 达标；
  dpkg-deb 由 pacman dpkg 包用户级解出：`~/bin/paleo-dpkg`）。
- GDAL：受控前缀 deb 闭包（manifest 记 3.13.3；以前缀实际为准，
  `SET_FROM_USER_INPUT_LIMITATIONS_get()` 已在 projectcoordinates.cpp 现用 → API 可用）。

## 工作包登记（当前调用链 / 缺口 / 本次实现 / 验收证据）

### W1 索引快照布局信息（v5 → v6）
- 现状：`SegyStoredIndex` 无布局标记。完整变道长索引可落盘并被 restore() 接受；
  checkpoint 审计用「末道偏移 + 全局固定道长」估算覆盖——变道长下既不充分也不正确。
- 缺口：固定/变道长/未知 marker 混为一谈；`decodePayload` 任意非 'C' marker 均按
  partial 解释；formatCode 未知时审计按 `qMax(1,bytes)` 每样 1 字节掩盖。
- 实现：v6 schema 增 `layoutKind`（'F' 固定 / 'V' 变长）+ 变长时逐道 `traceNs`
  （quint16[n]，有效 ns——UseBinary 记 binNs）。marker 仅接受 'C'/'P'；
  旧缓存（v5）按版本闸安全失效重建。禁止完整样本重扫冒充恢复（O(n) 纯算术验证）。
- 契约保持：变道长顺序可读、不落 checkpoint、取消不 rescan。

### W2 load/resume/restore 统一边界验证
- 现状：`load()` 有 `size+4096` 宽容（注释自认不该）；`loadForResume` 另一套；
  `restore()` 只验 `off+240<=fileSize`（#1026 G6：完整采样载荷无证明）。
- 实现：共用验证器（新增 `segyindexvalidate.{h,cpp}` 小文件，`SegyIndexAudit`）：
  减法/溢出安全；平行数组一致、条目数上限、严格递增；固定布局 offset 与
  `firstTraceOffset` 按道长同余 + 整道 span 在文件内；坏道 offset 同验并与好道
  合并全局严格递增（不重叠）；变长布局首道=firstTraceOffset、逐道
  `off[i+1]=off[i]+240+ns[i]*sb` 连续、末道端=complete?fileSize:scannedOffset；
  partial: `firstTraceOffset<=scannedOffset<=fileSize`（删 +4096）+ 同余 +
  ≥已覆盖末道；complete: scannedOffset==fileSize。未知 format/ns 越界直接失败。
  全部通过才替换 reader 内部状态（restore 现有次序保持）。

### W3 不可信缓存回退冷扫/续扫/取消
- 现状：openCached 已有「损坏→删→冷扫」「取消即返不重扫」「保存失败降级可用」
  骨架（tst 有 publishFailureStillUsable 等）。截短/替换判据在 loadForResume
  （size<scannedOffset 拒、inode/前缀指纹失配拒）已存在。
- 实现：W2 验证器接入后，审计失败一律「删缓存 + 可解释 miss + 冷扫重建」；
  load()/loadForResume()/restore() 三口同源。指纹仅首 64KB（不声称全文件哈希）
  ——注释如实写明。

### W4 GeoTIFF writer 独立成功发布契约
- 现状：`writeTimeSliceGeoTiff` 直接 GDALCreate(path) + 失败 `remove(path)`
  （#1024）；主服务传 `.partial` 缓解（保留事实，PR 里如实写）。
- 实现：writer 自管唯一暂存（`<path>.tmp<pid>_<rand>.tif`，同目录）：几何/轴/
  有限仿射前置验 → 全写入+元数据+SRS+GDALClose 检查 → 重开验证（尺寸/身份
  元数据/geotransform + 首末行像素回读）→ `paleoReplaceFile` 发布 +
  `paleoSyncDir`。任何失败只删自己暂存。服务层改传最终路径（单一职责，
  消除固定 `.partial` 嵌套）。

### W5 TIFF+provenance 完整缓存身份
- 现状：命中只核 sidecar 全量 hash；TIFF 内嵌 `PALEO_PARAM_HASH/ATTR_ID/
  SAMPLE_INDEX` 未与请求比对；summary `toInt/toDouble` 垃圾串静默变 0。
- 实现：命中需 sidecar hash==请求 && TIFF 内嵌 paramHash/attrId/sampleIndex
  与请求一致；summary 解析改 `ok` 位 + 有限值 + 维度>0 + validCells∈[0,nIl*nXl]
  + 时间/间隔非负有限 + geotransform 有限。不一致=miss（可重算），统计不入下游。
  文件名 hash12 只寻址不充身份（现有注释口径不变）。

### W6 缓存对并发/取消/失败恢复
- 现状：固定 `.partial`/`.prov.json.partial` 名并发互踩；昂贵计算后、发布前无
  取消复核；sidecar 失败在 TIFF 已替换后只报错（混合对靠「旧 sidecar≠新 hash」
  侥幸 miss）。
- 实现：唯一尝试标识（每次发布 attempt 的 tmp 后缀；sidecar 同）；发布前
  cancel-gate（取消→不产正式缓存及 sidecar，干净收尾）；TIFF 先、sidecar 后
  （双 rename 非跨文件 ACID——如实注明；混合对由 W5 双验判 miss，下次重算治愈）；
  提交后晚到取消：不删已发布缓存，cancelled 回执附既成缓存事实；失败只清本次
  拥有的暂存。

### W7 CRS 解析限制
- 三处：projectcoordinates `projectRasterFile` 的 `from`（栅格头元数据）、
  horizonbinner `writeGeoTiff`（工程 CRS；空→本地常量回退保留）、attrgridout
  （本地常量）。统一加 `SET_FROM_USER_INPUT_LIMITATIONS_get()`；
  horizonbinner 非空 CRS 解析失败由「静默无 SRS」改如实失败（文件/URL 形式明确
  拒绝；合法 WKT/EPSG/LOCAL_CS 行为不变）。

### W8 回归（真实生产入口 + 内部边界）
- 新增 `tests/tst_p2_seismic_cache.cpp`（writer 暂存/发布失败、混合对、summary
  抗垃圾）+ 扩展 tst_cache_segyindex（重封装磁盘缓存越过 CRC 的 tamper：超尾
  scannedOffset、道头在内载荷超尾、乱序/重叠、未知 marker/format、变长完整索引
  恢复等）+ 扩展 tst_seismicattrscan（取消闸/晚取消/sidecar 失败/坏缓存 miss 后
  重算二连中）+ tst_projectcoordinates/tst_horizonbinner CRS 回归。
- 夹具：合成 SEG-Y + 真实 GDAL TIFF；磁盘缓存变异后重算外层 CRC（cachecore 头）
  使验证器真正被测。

## 状态日志
- 2026-11-20 00:2x：核对完成（无开放 PR/相关分支仅 P6）；worktree 建好；
  vendor 接线（superbuild prefix/onnx/ranuts/cache symlink、Data_service@
  905b5c1b=master 钉住 gitlink）；deb 闭包 fetch 进行中。
- 2026-11-20 00:4x：依赖前缀落地过程（记录给复审）：
  - 主仓 `vendor/superbuild/prefix`（11-04 构建）无 #1042-V6 起要求的
    `.paleo-superbuild-base.sha256` 收据戳 → master 的 paleo-project-prefix
    闸 FATAL（不伪造戳）。
  - 按 BUILDING.md「Linux 二进制加速档」在本 worktree 跑
    `./paleo-dev bootstrap fetch-only`：glibc 2.44≥2.43 ✔；宿主缺 dpkg-deb，
    由 pacman 的 dpkg 包用户级解出（~/bin/paleo-dpkg，不入 PATH 全局）；
    915 debs → `vendor/prefix/usr`（deb 闭包锁校验过）。
  - Arch 的 gcc 不报 multiarch → `CMAKE_LIBRARY_ARCHITECTURE` 为空，
    显式 `-DCMAKE_LIBRARY_ARCHITECTURE=x86_64-linux-gnu`（前缀是 Ubuntu
    闭包，路径推导补齐，非绕过校验）。
  - 构建/运行测试须走 `./paleo-dev build/test` 的 env 包装（vendored Qt
    rcc/test 需 vendor/prefix 的 LD_LIBRARY_PATH；裸 ninja 会加载宿主
    libQt6Core 报 GNU_PROPERTY 冲突）。
- 2026-11-20 01:0x：W1-W7 生产代码 + W8 测试完成。
- 2026-11-20 01:2x：io 侧测试构建/首跑；修复自引入问题（QVector 空表
  fill 崩溃、目录隔离断言）；**存量红发现**：`tst_cache_segyindex::
  checkpointResumeAfterCancel` 在 master 上确定性红（10/10，本机
  cachyos + deb 前缀）——`cancel` 恒 true 时分片首查（i=0）即取消，
  m_index 为空 → `snapshot()` 正确拒绝落空索引 → 无 .psx。改为计数器
  取消（越过首个 128 道检查窗口）后 15/15 绿。**观察（不修，留档）**：
  顺序扫描取消路径 `open()` 失败时 `m_path` 未置（仅成功路径 line 715
  赋值）→ 小文件（<1MB）顺序取消不落 checkpoint（安全降级为整扫重来，
  非正确性问题；并行路径 m_path 前置故正常落盘）。
- 2026-11-20 01:3x：首轮全绿（tst_cache_segyindex 25/25、tst_p2_seismic_cache
  7/7、tst_seismicattrscan 18/18、CRS 两套、tst_perf_segyindex 10/10、
  tst_cache_io 26/26）+ 全部 Source gates；commit f4cac521 推送。
- 2026-11-20 14:0x：独立 review（未参与实现的 subagent，读真实 diff 并
  自行跑测试）→ BLOCKER×2 / MAJOR×1 / MINOR×7 / NIT×若干。处置：
  - **B1（真实生产缺陷，已修）**：`writeTimeSliceGeoTiff`/`readTimeSlice-
    GeoTiffSummary` 的裸 `GDALAllRegister()` 在任务 worker 线程上非线程
    安全——同 key 并发两扫描在 `GDALRegister_WMS` 里 SIGSEGV（review 实
    测 3/4 崩溃，本机复现 spy 超时形态）。改用仓内既有
    `paleo::io_detail::ensureGdalRegistered()`（call_once 幂等）。
  - **B2（测试缺陷，已修）**：并发用例两个 `spy.wait()` 顺序等待——Qt 的
    `QSignalSpy::wait` 只认未来发射，后完成的那个 `count()==1` 时 wait
    仍返回 false（实测确认），退化成 120s 超时。改为截止期限 + 泵事件
    轮询两个回调查看。
  - **M1（已补测）**：原取消用例都在计算前取消，新增
    `timeSliceMidScanCancelLeavesNoMixedPair`（bigVol_ 长扫描 + 进度
    轮询中途取消），按协议分叉断言：Cancelled→目录零产物；
    Succeeded（取消落闸后）→ 恰好一对一致缓存（sidecar hash 复核）。
    5/5 绿。
  - m1（已修）：summary 垃圾元数据用例改表驱动、每例独立成图，杜绝后例
    踩前例残值通过。
  - m3（已修）：writer/sidecar 暂存名由 `.paleo-tmp-*` 改为
    `.{name}.tmp.{pid}-{rand}`，并入 `PaleoAtomicTransaction::recover` 的
    `.{name}.tmp.*` 孤儿 glob——硬崩溃残留可被既有机制清扫。
  - m4（已修）：`paleoSyncDir` 失败补 qWarning（与仓内其它 replace 调用
    点口径一致）。
  - m5（已修）：`encodePayload` 对 'V' 布局 traceNs 长度不变式 fail-loud
    （空 payload → save 报错），杜绝 release OOB。
  - NIT（已修）：`checkedAdd` 补 `b<0` 防护。
  - m2/m6/m7 记录不修（见下「复审遗留」）。
- 2026-11-20 14:2x：复审修复后全量重跑（新进程，排除缓存假通过）：
  tst_seismicattrscan 19/19 ×2、tst_cache_segyindex 25/25、
  tst_p2_seismic_cache 7/7、tst_projectcoordinates 15+2skip、
  tst_horizonbinner 12+1skip、tst_perf_segyindex 10/10、tst_cache_io
  26/26；并发用例 8/8；Source gates 全绿。amend 提交后推送。

## 复审遗留（不属本任务范围或已评估不改）
- m2：writer 重开校验未逐键复校全部数值元数据（只验身份键 + geotransform
  + 首末行像素）。数值键在读侧有严格解析兜底，且 PALEO_VALID_CELLS 与
  实际非 nodata 计数的交叉核对留待缓存统计专项。
- m6：`startAttributeVolume`（.sattr）与 `writeHorizonGeoTiff` 仍用固定
  `.partial`——所有权表明令不改 startAttributeVolume 与层位算法/编排，
  其失败清理经 review 核对完整；同 key 并发互踩在体/层位路径仍存在，
  已在本账本与 PR 正文点名（不在本 PR 声称已覆盖）。
- m7：restore() 与 load() 的双重 O(n) 审计（纯算术，百万道索引约毫秒
  级，相对整秒级重扫可忽略），评估后不重构。

## 对 issue 的表述边界（PR 正文同）
- #1024：主服务原有 `.partial → replace` 缓解属实保留说明；本次新增的是
  writer 自身独立暂存/重开校验/原子发布 + sidecar 唯一暂存 + 取消闸，
  并消除固定 `.partial` 嵌套（两层职责收敛为一层）。
- #1025：统一策略口径（禁文件/网络访问），未做现实攻击复现，不使用
  未证实攻击措辞。
- #1026：未以「拒绝所有缓存」或「拒绝所有变道长」代替验证——合法完整/
  partial/变长/坏道/追加/方言索引全部保持可用（既有用例全绿为证）。
- 双文件 replace 未描述为跨文件 ACID；缓存首段指纹未描述为全文件哈希。
