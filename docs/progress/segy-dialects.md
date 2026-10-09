# segy-dialects — SEG-Y 道头方言与号域退化根治（实战系补账立账）

本文为补账文档：e9d17e80「并行 SEG-Y 扫描补齐 demo 工区方言，号域不再
退化」（src/io/segyreader.cpp +186/-76、segyindexstore.cpp v3→v4）此前
零文档。行号为 origin/master `f31ee461` 口径。

## What（交付面）

| 面 | 内容 |
|----|------|
| 方言背景 | demo 工区 `200P_seismic.sgy`（966MB）道头方言：crossline 字节 193 恒 0，号域与坐标只存在于 CDP（21）与 CDP X/Y（181-188）——docx 道头契约「道号位置 21 / X=181 / Y=185」（segyreader.cpp:88-91, :308 注释） |
| 故障形态 | 方言回退此前只实现在顺序 `open()`；>1MB 体走 `openCached()` 并行扫描时仍按标准字节位读 → xline 全 0、四角全 0 写进索引缓存 → 地震聚类前置校验「地震体号域不构成二维测网」失败（seismichorizoncluster.cpp:82-84 即该闸门）（e9d17e80 提交说明） |
| 修复 | 探针（probeFieldVaries/fieldAllZero/pairHasNonZero）提取为命名空间级共用函数，open() 与 openCached() 并行路径同一判据不漂移（segyreader.cpp:88-96）；scanParallel 增 xlineFromCdp/cornerFromCdpXY 参数分片选字节位（:990） |
| 缓存自愈 | 索引缓存版本 v3→v4：旧退化缓存过版自愈重建（segyindexstore.cpp:19-20, :192/:254 损坏/过版即删） |

## 口径（语义决策）

1. **方言判据（共用探针）**：
   - `ordinalIndex` = inline 字段不变 → 道号按序推导（:305）；
   - `xlineFromCdp` = 非 ordinal 且 crossline 字段恒 0 且 CDP 字段变化
     （:320-322）——xline 取 CDP 值（:466）；
   - `cornerFromCdpXY` = 源点坐标（72-79）恒 0 且 CDP X/Y（180-187）
     有非零对（:323）——角点坐标取 181-188（:424-425）；
   - 探针抽查首段 ≤4096 道，负 ns 按坏道跳过（与主扫描同语义，:92-95）。
2. **顺序/并行同一判据是硬约束**：两条路径判据漂移 = 方言体只在一条
   扫得全——号域退化即由此而来（:90-91 注释原话）；探针共用实现是
   防漂移的机制化。
3. **缓存版本即方言代际**：v4 与判据改动同提交落——判据演进必须过版
   （老缓存语义可能已错，静默复用会固化退化号域）。
4. **道头偏移经 AreaRules 可调**：inline/crossline/cdpXline 偏移来自
   `SegyIndexing`（domain/arearules.h:47-52，默认 188/192/20），第二工区
   方言经 project_area.json 配置（segyreader.cpp:298-299 注释）——
   代码内方言探针只兜「字段恒 0」这类结构退化，不硬编码工区特例。

## 证据

- 回归测试：tst_segy `dialectLayoutParallelOpenCachedMatchesSequential`
  （tests/tst_segy.cpp:609+）——方言体 >1MB（3000 道 ≈1.49MB 并行资格）
  顺序/并行/缓存热读三方 inline/xline 范围与四角坐标一致，且钉死
  corner[2] = (10·(perLine-1), 20·(lines-1))。
- 合成体夹具带方言布局注释（tst_segy.cpp:139-144「坐标只出现在
  CDP X/Y (181-188)，源点 73-78 与 crossline 193-196 保持全零」）。
- 下游闸门：seismichorizoncluster.cpp:81-100（号域二维测网校验 +
  SurveyGridGeometry 构建）——方言修复后 demo 工区聚类链可跑通。

## 对账与递延

- 与地震聚类（seismichorizoncluster）：**依赖**——聚类的测网定位/
  取窗全部建立在 reader 几何之上；号域退化是聚类故障的直接根因
  （本方向另篇 docs/progress/seismichorizoncluster.md 交叉引用）。
- 与方向 23 属性体/seismic-3d（同一 SegyReader 消费者）：**正交但同
  受益**——方言修复对全部 openCached 消费者生效，无各自适配。
- 与 D2.3 索引缓存自愈机制（segyindexstore 损坏/过版即删）：**依赖**——
  v4 过版复用该机制，未加新路径。
- 递延：方言探针抽查首段 4096 道——若方言标记在文件后段切换（混合
  采集）理论上误判，当前无此类工区数据；记 TODOS 观察项。
