# Full Audit 2026-10-22 — 03 调用链与契约（增量窗主链）

本轮深审建立的增量窗调用链结论（证据在各分报告；此处收口跨模块主链）。

## 统一测井读取链（#603 核心，A/C 复核）

    detect(path) ─┬─ Las/Dlis/Lis → 既有 parser（LasCache 缓存键=path+mtime+size）
                  ├─ SpreadsheetXml/Text → WellLogText::parse（64MB/4M 格/256 列预算）
                  └─ Unknown → parseAdditional 拒收（诚实失败）
    消费方六处（wellsectionworkflow / wellpredictionservice worker /
    mappingworkbench_predict / propertymodelworkflow / wellpredictionreview /
    sectionworkbench）——C-8 修复后每调用点一次探测。
    井名提取：ingestplan::proposeWellLog → readWellInfo（Text/XML 走全量验证）。

## 导入事务链（#603，A 深审 + 6 例崩溃注入测试对账）

    previewFolder（plan：分类器提议 + sha 指纹 + F1 预览集）
      → beginImport/produceFromPlan（F1 迟到行 Skipped 缀尾[A-08 修]）
      → produceItem（staging 副本 + expected sha 复核）
      → writeImportIntent（清单+逐文件 sha256 原子落盘）
      → commitImport（catalog 事务：epoch/baseSeq 冲突重 produce ≤kMax、
        原子 rename → journal 重放 → 双路回滚 rollbackFailedRow）
      → 再开工程 recoverImportIntents（在册保护 + sha 复核 + symlink 拒绝）。
    取消语义：cancelled 门 + finished 复核 = 整批丢弃（有意设计，有测试）。

## 深度单位/基准契约（簇 1 修复后的统一口径）

    权威词表 paleo::wellnumeric::depthScaleToMetres：M 族=1.0、FT 族=0.3048、
    其余 nullopt（空单位语义按调用面显式声明）。
    - 预测管线 normalizeDepthCurve（welllogfacies 变体）：空=米。
    - predictionLog（C-7 修）：全格式归一，空=米（与入库侧对齐），未知非空拒收。
    - mdAxisInMetres（B-02 修）：legacy 空=米惯例，未知非空拒收；非 legacy 空/未知拒收。
    - petrophys additionalLogGridIsMd：非 text/xml 直通 true；basis 空则 parseHeader。
    已知残留三体（LasParser::depthUnitToMeters 0.0 语义）→ 观察项 6，结转。

## 任务调度链（B-01 修复后）

    PaleoTaskService 专用池（≤8 线程，QThreadPool 优先级）
      ├─ JobRunner::start(priority)（默认 High；BatchQueue 传 Low）
      ├─ start 5 参（显式 priority）；3/4 参默认 Normal（本轮落实）
      ├─ beginExternal（不占池）
      └─ BatchQueue pump：让位闸（非本批 runningCount ≥ poolCap 停出队 +
        pumpLater 补位），槽位/熔断/续跑/会话守卫不变。

## 预览栅格分析链（B-05 修复后）

    requestSummary/Histogram/Extrema/Profile（tasks 空=同步；否则任务 +
      mtime key 闸 + 两级缓存）→ finished 恒回调（成功=载荷+缓存；失败/取消/
      key 变更=default 载荷）→ 消费方四面板（tocpanel 清值+tooltip /
      tabhorizon、tabmapproduct「不可用」/ 直方图保留渲染）。

## 综合柱状图道身份链（D-01 修复后）

    defaultlayout（Layers/Sequence/SandGroup 三道标注）
      ↔ TrackSpec.params.formationSource（registry 序列化，缺省不写=旧文件兼容）
      ↔ trackops 复制保持身份
      → paneltracks syncSessionToTracks 按身份路由（编辑会话 documentChanged）。
    标题仅展示（用户可改名）；会话记忆按标题存取不变（自洽）。
