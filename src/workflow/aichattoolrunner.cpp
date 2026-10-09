// 层：功能
#include "aichattoolrunner.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QFileInfo>
#include <QJsonValue>
#include <QMetaObject>
#include <QPointer>

#include <algorithm>

#include "../ai/chat/domaintools.h"
#include "../ai/wellfaciesservice.h"
#include "../catalog/datacatalog.h"
#include "../domain/welllogfacies.h"
#include "../services/derivationgraph.h"
#include "../services/paleotaskservice.h"

// workflow/ — 聊天工具执行器实现（方向61；方向77 增只读查询/血缘两工具）。
//
// 结果 JSON 契约：成功 = {"tool": <名>, ...摘要}；失败 = {"tool": <名>,
// "error": <人可读原因>}。两种都按 role=tool 回灌——协议要求每个
// tool_call_id 有应答，错误也让模型如实转述，不在本地吞掉。

namespace {
// 聊天面的 tile 分类低置信阈值：工具 schema 不暴露该参数（表 shape 与方向51
// 一致），执行面取 0.5（掩膜产品语义见 aiassistworkflow.h）。想调参走
// 工作流面板，不走聊天。
constexpr double kChatLowConfidenceThreshold = 0.5;

// 方向77 出参列表上限（ledger 定案）：结构化截断优先于全局 4000 字符 clamp
// ——JSON 保持合法，总数如实另报。
constexpr int kQueryListCap = 50;
constexpr int kWellLinksCap = 100;
// 摘要段硬上限（预算纪律：system prompt 轻注入不喧宾夺主）。
constexpr int kBriefCharCap = 300;
constexpr int kBriefAssetTypes = 6;

int intArg(const QJsonObject &args, const char *key, int fallback) {
  const QJsonValue value = args.value(QLatin1String(key));
  if (value.isDouble())
    return qRound(value.toDouble());
  if (value.isString()) // 表口径允许 "12" 文本整数（jsonTypeMatches）
    return value.toString().trimmed().toInt();
  return fallback;
}

double doubleArg(const QJsonObject &args, const char *key) {
  const QJsonValue value = args.value(QLatin1String(key));
  if (value.isDouble())
    return value.toDouble();
  if (value.isString()) {
    bool ok = false;
    const double parsed = value.toString().trimmed().toDouble(&ok);
    if (ok)
      return parsed;
  }
  return qQNaN(); // 未给
}
} // namespace

AiChatToolRunner::AiChatToolRunner(QObject *parent)
  : QObject(parent), m_facies(new WellFaciesService(this)) {}

AiChatToolRunner::~AiChatToolRunner() {
  ++m_generation; // 作废在途回调
  m_queue.clear();
  if (m_activeTask)
    m_activeTask->requestCancel();
  if (m_activeIsFacies)
    m_facies->cancel();
}

void AiChatToolRunner::setWorkflow(AiAssistWorkflow *workflow) {
  m_workflow = workflow;
}

void AiChatToolRunner::setFaciesConfig(const WellFaciesConfig &config) {
  m_faciesConfig = config;
  m_facies->configure(config);
}

void AiChatToolRunner::setContext(const Context &context) {
  m_context = context;
}

void AiChatToolRunner::run(const ChatToolCall &call) {
  m_queue.append(call);
  startNext();
}

void AiChatToolRunner::cancel() {
  // 取消 = 作废（口径见头文件）：在途结果不再 emit；tile 任务协作取消
  // （不写产品）；层位任务算完但丢弃；测井相 HTTP 中止。
  ++m_generation;
  m_queue.clear();
  if (m_activeTask)
    m_activeTask->requestCancel();
  if (m_activeIsFacies) {
    m_facies->disconnect(this);
    m_facies->cancel();
  }
  m_active = false;
  m_activeIsFacies = false;
  m_activeTask.clear();
}

void AiChatToolRunner::startNext() {
  if (m_active || m_queue.isEmpty())
    return;
  m_activeCall = m_queue.takeFirst();
  m_active = true;
  m_activeIsFacies = false;
  emit toolStarted(m_activeCall);

  QJsonParseError parseError;
  const QJsonDocument document =
    QJsonDocument::fromJson(m_activeCall.argumentsJson.toUtf8(), &parseError);
  const QJsonObject args = document.object();
  const AiToolDispatch dispatch = dispatchAiTool(m_activeCall.name, args);
  // 表驱动诚实态：未登记 / 本构建不可用 / 实参不合格 → 错误结果回灌
  // （不冒充成功，也不静默丢帧）。
  if (parseError.error != QJsonParseError::NoError && !document.isNull()) {
    finishActive(false, errorPayload(tr("实参不是合法 JSON：%1")
                                       .arg(parseError.errorString())));
    return;
  }
  if (dispatch.status != AiToolDispatchStatus::Routed) {
    const QString text = aiToolDispatchLabel(dispatch.status) +
                         (dispatch.note.isEmpty()
                            ? QString()
                            : QStringLiteral("：%1").arg(dispatch.note));
    finishActive(false, errorPayload(text));
    return;
  }
  if (m_activeCall.name == QStringLiteral("paleo.tile_classification"))
    runTile(m_activeCall, args);
  else if (m_activeCall.name ==
           QStringLiteral("paleo.horizon_tracking_suggestion"))
    runHorizon(m_activeCall, args);
  else if (m_activeCall.name == QStringLiteral("paleo.well_facies_prediction"))
    runFacies(m_activeCall, args);
  else if (m_activeCall.name == QStringLiteral("paleo.query_project"))
    runQuery(m_activeCall, args);
  else if (m_activeCall.name == QStringLiteral("paleo.asset_lineage"))
    runLineage(m_activeCall, args);
  else
    finishActive(false, errorPayload(tr("未登记的工具 %1").arg(m_activeCall.name)));
}

QJsonObject AiChatToolRunner::errorPayload(const QString &message) const {
  QJsonObject payload;
  payload.insert(QStringLiteral("tool"), m_activeCall.name);
  payload.insert(QStringLiteral("error"), message);
  return payload;
}

void AiChatToolRunner::finishActive(bool ok, const QJsonObject &payload) {
  const ChatToolCall call = m_activeCall;
  const bool wasFacies = m_activeIsFacies;
  const int generation = m_generation;
  m_active = false;
  m_activeIsFacies = false;
  m_activeTask.clear();
  if (wasFacies)
    m_facies->disconnect(this); // 单发钩子：用完即拆（串行执行下防堆积）
  // 统一异步回报（#281）：同步失败分支（实参非法/未路由/上下文未绑定）若
  // 从 run() 的调用栈直接 emit，调用方（aichatcontroller 的派发循环）会在
  // 同一栈里被重入改容器——range-for 迭代器失效 UB。toolFinished 一律经
  // 事件循环再发，成功/失败同一语义；cancel 经世代号把在途报告作废（取消=
  // 作废，口径见头文件），对象析构时 Qt 随 context 丢弃未投递的入队调用。
  QMetaObject::invokeMethod(
    this,
    [this, generation, call, ok, payload]() {
      if (generation != m_generation)
        return; // 已取消/作废：在途结果不再回灌
      emit toolFinished(call, ok,
                        QString::fromUtf8(QJsonDocument(payload).toJson(
                          QJsonDocument::Compact)));
      startNext();
    },
    Qt::QueuedConnection);
}

void AiChatToolRunner::runTile(const ChatToolCall &call,
                               const QJsonObject &args) {
  Q_UNUSED(call)
  if (!m_workflow) {
    finishActive(false, errorPayload(tr("tile 分类执行面未绑定（AiAssistWorkflow）")));
    return;
  }
  if (m_context.horizon.isEmpty() || !m_context.gridFetch) {
    finishActive(false,
                 errorPayload(tr("tile 分类上下文未绑定（须打开工程并提供网格取数）")));
    return;
  }
  const QString model = args.value(QStringLiteral("model")).toString();
  const int rows = intArg(args, "grid_rows", 0);
  const int cols = intArg(args, "grid_columns", 0);
  if (model.isEmpty() || rows <= 0 || cols <= 0) {
    finishActive(false, errorPayload(tr("模型名与正数网格尺寸为必填")));
    return;
  }
  const int tileRows = qMax(1, intArg(args, "tile_rows", 128));
  const int tileCols = qMax(1, intArg(args, "tile_columns", 128));
  const int halo = qMax(0, intArg(args, "halo", 8));
  QString error;
  PaleoTask *task = m_workflow->startClassification(
    m_context.horizon, model, rows, cols, tileRows, tileCols, halo,
    m_context.gridFetch, kChatLowConfidenceThreshold, &error);
  if (!task) {
    finishActive(false, errorPayload(error));
    return;
  }
  m_activeTask = task;
  const int generation = m_generation;
  const QString callId = m_activeCall.id;
  connect(task, &PaleoTask::finished, this, [this, generation, callId]() {
    if (generation != m_generation || !m_active || m_activeCall.id != callId)
      return; // 已被取消/作废或串台：迟到终态不回灌
    const PaleoTask::State state = m_activeTask->state();
    if (state == PaleoTask::State::Succeeded) {
      QJsonObject payload = m_workflow->lastProductStats();
      payload.insert(QStringLiteral("tool"),
                     QStringLiteral("paleo.tile_classification"));
      payload.insert(QStringLiteral("horizon"), m_context.horizon);
      payload.insert(
        QStringLiteral("note"),
        tr("产物已按草稿态登记为 DERIVED 派生资产（03_Predict 组），"
           "未改任何解释数据"));
      finishActive(true, payload);
      return;
    }
    if (state == PaleoTask::State::Cancelled)
      finishActive(false, errorPayload(tr("tile 分类已取消（未写任何产品）")));
    else
      finishActive(false, errorPayload(m_activeTask->errorText()));
  });
}

void AiChatToolRunner::runHorizon(const ChatToolCall &call,
                                  const QJsonObject &args) {
  Q_UNUSED(call)
  if (!m_workflow) {
    finishActive(false, errorPayload(tr("层位建议执行面未绑定（AiAssistWorkflow）")));
    return;
  }
  if (!m_context.traceFetch) {
    finishActive(false,
                 errorPayload(tr("层位建议上下文未绑定（须提供道窗取数）")));
    return;
  }
  const QString model = args.value(QStringLiteral("model")).toString();
  const int windowSamples = intArg(args, "window_samples", 0);
  const int radius = intArg(args, "radius", 0);
  const QJsonArray seeds = args.value(QStringLiteral("seeds")).toArray();
  QVector<TrackingSeed> parsed;
  for (const QJsonValue &value : seeds) {
    const QJsonObject seed = value.toObject();
    TrackingSeed s;
    s.inlineNo = intArg(seed, "inline", 0);
    s.xlineNo = intArg(seed, "xline", 0);
    s.sampleIndex = intArg(seed, "sample_index", 0);
    parsed.append(s);
  }
  if (model.isEmpty() || windowSamples <= 0 || radius <= 0 || parsed.isEmpty()) {
    finishActive(false,
                 errorPayload(tr("模型名、正数窗长/半径与至少一个种子点为必填")));
    return;
  }
  QString error;
  PaleoTask *task = m_workflow->startSuggestion(
    m_context.horizon, model, parsed, windowSamples, radius,
    m_context.traceFetch, &error);
  if (!task) {
    finishActive(false, errorPayload(error));
    return;
  }
  m_activeTask = task;
  const int generation = m_generation;
  const QString callId = m_activeCall.id;
  connect(task, &PaleoTask::finished, this, [this, generation, callId]() {
    if (generation != m_generation || !m_active || m_activeCall.id != callId)
      return;
    const PaleoTask::State state = m_activeTask->state();
    if (state == PaleoTask::State::Succeeded) {
      // 建议已入裁决队列（startSuggestion 收尾）：数量从队列侧取——
      // 非种子未裁决 + 已接受（含种子锚点）= 总数。
      const int pending = m_workflow->pendingCount(m_context.horizon);
      const int accepted = m_workflow->acceptedCount(m_context.horizon);
      int seeds = 0;
      for (const TrackingSuggestion &s :
           m_workflow->acceptedSuggestions(m_context.horizon))
        if (s.isSeed)
          ++seeds;
      QJsonObject payload;
      payload.insert(QStringLiteral("tool"),
                     QStringLiteral("paleo.horizon_tracking_suggestion"));
      payload.insert(QStringLiteral("horizon"), m_context.horizon);
      payload.insert(QStringLiteral("suggestions_total"), pending + accepted);
      payload.insert(QStringLiteral("pending_review"), pending);
      payload.insert(QStringLiteral("seed_count"), seeds);
      payload.insert(
        QStringLiteral("note"),
        tr("建议已进入待裁决队列（解释员逐条接受/否决，显式提交才落库；"
           "未自动写任何解释）"));
      finishActive(true, payload);
      return;
    }
    if (state == PaleoTask::State::Cancelled)
      finishActive(false, errorPayload(tr("层位建议已取消（建议未入队）")));
    else
      finishActive(false, errorPayload(m_activeTask->errorText()));
  });
}

void AiChatToolRunner::runFacies(const ChatToolCall &call,
                                 const QJsonObject &args) {
  Q_UNUSED(call)
  const QString endpointError =
    WellFaciesConfig::validateUrl(m_faciesConfig.baseUrl,
                                  m_faciesConfig.allowInsecureHttp);
  if (!endpointError.isEmpty() || m_faciesConfig.apiKey.isEmpty()) {
    finishActive(false,
                 errorPayload(tr("测井相服务未配置（端点与密钥须显式设置）")));
    return;
  }
  if (!m_context.faciesInput) {
    finishActive(false,
                 errorPayload(tr("未绑定井曲线数据上下文（须打开工程并加载井数据）")));
    return;
  }
  const QString modelId = args.value(QStringLiteral("model")).toString();
  const QString well = args.value(QStringLiteral("well_name")).toString();
  const double depthFrom = doubleArg(args, "depth_from");
  const double depthTo = doubleArg(args, "depth_to");
  if (modelId.isEmpty() || well.isEmpty()) {
    finishActive(false, errorPayload(tr("模型 id 与井名为必填")));
    return;
  }
  WellFaciesInput input;
  QString inputError;
  if (!m_context.faciesInput(well, depthFrom, depthTo, &input, &inputError)) {
    finishActive(false, errorPayload(inputError));
    return;
  }
  if (!input.ready()) {
    finishActive(false, errorPayload(input.reason));
    return;
  }
  WellFaciesModel model;
  if (m_context.faciesModel)
    model = m_context.faciesModel(modelId);
  model.id = modelId;
  m_activeIsFacies = true;
  const int generation = m_generation;
  const QString callId = m_activeCall.id;
  connect(m_facies, &WellFaciesService::completed, this,
          [this, generation, callId, well](const WellFaciesResult &result) {
            if (generation != m_generation || !m_active ||
                m_activeCall.id != callId)
              return;
            QJsonObject payload;
            payload.insert(QStringLiteral("tool"),
                           QStringLiteral("paleo.well_facies_prediction"));
            payload.insert(QStringLiteral("well"), well);
            payload.insert(QStringLiteral("model"), result.modelName);
            payload.insert(QStringLiteral("intervals"),
                           int(result.intervals.size()));
            QJsonArray sample;
            const int kSample = 20; // 摘要上限：全文给模型只会烧上下文
            for (int i = 0; i < result.intervals.size() && i < kSample; ++i) {
              const auto &interval = result.intervals[i];
              QJsonObject entry;
              entry.insert(QStringLiteral("top"), double(interval.topDepth));
              entry.insert(QStringLiteral("bottom"),
                           double(interval.bottomDepth));
              entry.insert(QStringLiteral("category"), interval.category);
              sample.append(entry);
            }
            payload.insert(QStringLiteral("intervals_sample"), sample);
            if (result.intervals.size() > kSample)
              payload.insert(
                QStringLiteral("truncated"),
                tr("仅前 %1 段（共 %2 段）").arg(kSample).arg(result.intervals.size()));
            finishActive(true, payload);
          });
  connect(m_facies, &WellFaciesService::failed, this,
          [this, generation, callId](const QString &reason) {
            if (generation != m_generation || !m_active ||
                m_activeCall.id != callId)
              return;
            finishActive(false, errorPayload(reason));
          });
  m_facies->predict(model, well, input);
}

// ---------------------------------------------------------------------------
// 方向77：只读工程查询（query_project / asset_lineage）
// ---------------------------------------------------------------------------

namespace {
// 资产条目的版本引用（可追溯口径）：无版本记录时字段留空——如实，不编造。
void insertVersionCitation(QJsonObject *entry, const DataCatalog *catalog,
                           const QString &assetId) {
  const CatalogVersion version = catalog->currentVersion(assetId);
  entry->insert(QStringLiteral("version_id"), version.id);
  entry->insert(QStringLiteral("stage"), version.stage);
  entry->insert(QStringLiteral("version_number"), version.versionNumber);
  entry->insert(QStringLiteral("path"), version.path);
}

QString displayName(const CatalogAsset &asset) {
  return asset.displayName.isEmpty() ? asset.id : asset.displayName;
}
} // namespace

void AiChatToolRunner::runQuery(const ChatToolCall &call,
                                const QJsonObject &args) {
  Q_UNUSED(call)
  if (!m_context.catalog || !m_context.catalog->isOpen()) {
    finishActive(false,
                 errorPayload(tr("未绑定工程目录（须先打开工程再查工程数据）")));
    return;
  }
  const DataCatalog *catalog = m_context.catalog;
  const QString topic = args.value(QStringLiteral("topic")).toString();

  QJsonObject payload;
  payload.insert(QStringLiteral("tool"), QStringLiteral("paleo.query_project"));
  payload.insert(QStringLiteral("topic"), topic);
  payload.insert(QStringLiteral("project_dir"), m_context.projectDir);

  if (topic == QStringLiteral("summary")) {
    // 先落局部再迭代——entityCountsByType 按值返回，begin/end 各取一个
    // 临时对象是悬空迭代（UB）。
    const QHash<QString, int> entityCounts = catalog->entityCountsByType();
    QJsonObject entities;
    for (auto it = entityCounts.cbegin(); it != entityCounts.cend(); ++it)
      entities.insert(it.key(), it.value());
    QHash<QString, int> assetCounts;
    for (const CatalogAsset &asset : catalog->assets())
      ++assetCounts[asset.type];
    QJsonObject assets;
    for (auto it = assetCounts.cbegin(); it != assetCounts.cend(); ++it)
      assets.insert(it.key(), it.value());
    payload.insert(QStringLiteral("entities"), entities);
    payload.insert(QStringLiteral("assets_by_type"), assets);
    payload.insert(QStringLiteral("revision"), catalog->catalogRevision());
    finishActive(true, payload);
    return;
  }

  if (topic == QStringLiteral("wells")) {
    // 井清单 = 井族实体（well + planned；计划井语义由条目 type 区分——
    // 测线/界面/辅助属别的实体类型，不在「有哪些井」的回答里掺沙）。
    QVector<CatalogEntity> wellLike;
    for (const CatalogEntity &entity : catalog->entities())
      if (entity.entityType == QStringLiteral("well") ||
          entity.entityType == QStringLiteral("planned"))
        wellLike.append(entity);
    std::sort(wellLike.begin(), wellLike.end(),
              [](const CatalogEntity &a, const CatalogEntity &b) {
                const QString an = a.name.isEmpty() ? a.id : a.name;
                const QString bn = b.name.isEmpty() ? b.id : b.name;
                return an < bn;
              });
    QJsonArray wells;
    int truncatedAt = -1;
    for (int i = 0; i < wellLike.size(); ++i) {
      if (wells.size() >= kQueryListCap) {
        truncatedAt = i;
        break;
      }
      QJsonObject entry;
      entry.insert(
        QStringLiteral("name"),
        wellLike[i].name.isEmpty() ? wellLike[i].id : wellLike[i].name);
      entry.insert(QStringLiteral("id"), wellLike[i].id);
      entry.insert(QStringLiteral("type"), wellLike[i].entityType);
      entry.insert(QStringLiteral("td"), wellLike[i].td);
      wells.append(entry);
    }
    payload.insert(QStringLiteral("count"), wellLike.size());
    payload.insert(QStringLiteral("wells"), wells);
    if (truncatedAt >= 0)
      payload.insert(QStringLiteral("truncated"),
                     tr("仅前 %1 条（共 %2 条）")
                       .arg(wells.size())
                       .arg(wellLike.size()));
    finishActive(true, payload);
    return;
  }

  if (topic == QStringLiteral("horizons") || topic == QStringLiteral("assets")) {
    const QString typeFilter =
      topic == QStringLiteral("horizons")
        ? QStringLiteral("horizon")
        : args.value(QStringLiteral("asset_type")).toString();
    QVector<CatalogAsset> matched;
    for (const CatalogAsset &asset : catalog->assets())
      if (typeFilter.isEmpty() || asset.type == typeFilter)
        matched.append(asset);
    std::sort(matched.begin(), matched.end(),
              [](const CatalogAsset &a, const CatalogAsset &b) {
                return displayName(a) < displayName(b);
              });
    QJsonArray entries;
    int truncatedAt = -1;
    for (int i = 0; i < matched.size(); ++i) {
      if (entries.size() >= kQueryListCap) {
        truncatedAt = i;
        break;
      }
      QJsonObject entry;
      entry.insert(QStringLiteral("name"), displayName(matched[i]));
      if (topic == QStringLiteral("horizons"))
        entry.insert(QStringLiteral("asset_id"), matched[i].id);
      else {
        entry.insert(QStringLiteral("id"), matched[i].id);
        entry.insert(QStringLiteral("type"), matched[i].type);
        entry.insert(QStringLiteral("format"), matched[i].format);
      }
      insertVersionCitation(&entry, catalog, matched[i].id);
      entries.append(entry);
    }
    payload.insert(QStringLiteral("count"), matched.size());
    payload.insert(topic == QStringLiteral("horizons")
                     ? QStringLiteral("horizons")
                     : QStringLiteral("assets"),
                   entries);
    if (truncatedAt >= 0)
      payload.insert(QStringLiteral("truncated"),
                     tr("仅前 %1 条（共 %2 条）")
                       .arg(entries.size())
                       .arg(matched.size()));
    finishActive(true, payload);
    return;
  }

  if (topic == QStringLiteral("well_details")) {
    const QString wellName = args.value(QStringLiteral("well_name")).toString();
    if (wellName.isEmpty()) {
      finishActive(false,
                   errorPayload(tr("topic=well_details 须给 well_name（井名）")));
      return;
    }
    const QStringList candidates = catalog->wellsMatchingName(wellName);
    if (candidates.isEmpty()) {
      // 如实报错并给出实证井名（截 10 个）——模型可据此向用户澄清。
      QStringList known;
      for (const CatalogEntity &entity : catalog->entities(QStringLiteral("well")))
        known.append(entity.name.isEmpty() ? entity.id : entity.name);
      known.sort();
      finishActive(false,
                   errorPayload(tr("井不存在：%1；工程现有井：%2")
                                  .arg(wellName,
                                       known.size() > 10
                                         ? known.mid(0, 10).join(
                                             QStringLiteral("、")) + QStringLiteral("…")
                                         : known.join(QStringLiteral("、")))));
      return;
    }
    if (candidates.size() > 1) {
      finishActive(false,
                   errorPayload(tr("井名匹配到多个候选（%1）——请给更精确的井名")
                                  .arg(candidates.join(QStringLiteral("、")))));
      return;
    }
    const CatalogEntity well = catalog->entityById(candidates.first());
    const QString roleFilter = args.value(QStringLiteral("role")).toString();
    QVector<EntityAssetLink> links = catalog->linksForEntity(well.id);
    QJsonArray entries;
    int truncatedAt = -1;
    for (int i = 0; i < links.size(); ++i) {
      if (!roleFilter.isEmpty() && links[i].role != roleFilter)
        continue;
      if (entries.size() >= kWellLinksCap) {
        truncatedAt = i;
        break;
      }
      const CatalogAsset asset = catalog->assetById(links[i].assetId);
      QJsonObject entry;
      entry.insert(QStringLiteral("role"), links[i].role);
      entry.insert(QStringLiteral("asset"), displayName(asset));
      entry.insert(QStringLiteral("asset_id"), links[i].assetId);
      entry.insert(QStringLiteral("primary"), links[i].isPrimary);
      insertVersionCitation(&entry, catalog, links[i].assetId);
      entries.append(entry);
    }
    payload.insert(QStringLiteral("well"),
                   well.name.isEmpty() ? well.id : well.name);
    payload.insert(QStringLiteral("well_id"), well.id);
    payload.insert(QStringLiteral("links"), entries);
    if (truncatedAt >= 0)
      payload.insert(QStringLiteral("truncated"),
                     tr("仅前 %1 条关联（共 %2 条）")
                       .arg(entries.size())
                       .arg(links.size()));
    finishActive(true, payload);
    return;
  }

  finishActive(false,
               errorPayload(tr("未知 topic：%1（合法值见工具 schema enum）")
                              .arg(topic)));
}

void AiChatToolRunner::runLineage(const ChatToolCall &call,
                                  const QJsonObject &args) {
  Q_UNUSED(call)
  if (!m_context.catalog || !m_context.catalog->isOpen()) {
    finishActive(false,
                 errorPayload(tr("未绑定工程目录（须先打开工程再查血缘）")));
    return;
  }
  const DataCatalog *catalog = m_context.catalog;
  const QString assetKey = args.value(QStringLiteral("asset")).toString();

  // 解析序（ledger 定案）：versionId → assetId → displayName 唯一命中。
  QString seedVersionId;
  QString resolvedAssetId;
  if (catalog->versionById(assetKey).id == assetKey) {
    seedVersionId = assetKey;
    resolvedAssetId = catalog->versionById(assetKey).assetId;
  } else if (catalog->assetById(assetKey).id == assetKey) {
    resolvedAssetId = assetKey;
    seedVersionId = catalog->currentVersion(assetKey).id;
  } else {
    QVector<CatalogAsset> byName;
    for (const CatalogAsset &asset : catalog->assets())
      if (asset.displayName == assetKey)
        byName.append(asset);
    if (byName.size() > 1) {
      finishActive(false,
                   errorPayload(tr("资产名匹配到多个候选（%1）——请给 asset id")
                                  .arg(assetKey)));
      return;
    }
    if (byName.size() == 1) {
      resolvedAssetId = byName.first().id;
      seedVersionId = catalog->currentVersion(resolvedAssetId).id;
    }
  }
  if (seedVersionId.isEmpty()) {
    finishActive(false,
                 errorPayload(tr("未找到资产或其版本记录：%1（可用 "
                                 "paleo.query_project topic=assets 查资产清单）")
                                .arg(assetKey)));
    return;
  }

  paleo::derivation::Query query;
  query.versionId = seedVersionId;
  query.upstreamDepth = qBound(0, intArg(args, "upstream_depth", 3), 12);
  query.downstreamDepth = qBound(0, intArg(args, "downstream_depth", 1), 12);
  const paleo::derivation::Graph graph =
    paleo::derivation::Service::build(catalog, query);
  const QSet<QString> closure =
    paleo::derivation::Service::selectionClosure(catalog, graph, seedVersionId);

  QJsonObject payload;
  payload.insert(QStringLiteral("tool"), QStringLiteral("paleo.asset_lineage"));
  payload.insert(QStringLiteral("asset"), assetKey);
  payload.insert(QStringLiteral("asset_id"), resolvedAssetId);
  payload.insert(QStringLiteral("version_id"), seedVersionId);
  payload.insert(QStringLiteral("available"), graph.available);
  QJsonArray nodes;
  for (const paleo::derivation::Node &node : graph.nodes) {
    QJsonObject entry;
    entry.insert(QStringLiteral("version_id"), node.versionId);
    entry.insert(QStringLiteral("asset"), node.assetName);
    entry.insert(QStringLiteral("asset_id"), node.assetId);
    entry.insert(QStringLiteral("stage"), node.stage);
    entry.insert(QStringLiteral("kind"),
                 node.kind == paleo::derivation::Kind::Raw
                   ? QStringLiteral("raw")
                   : node.kind == paleo::derivation::Kind::Derived
                       ? QStringLiteral("derived")
                       : QStringLiteral("external"));
    entry.insert(QStringLiteral("stale"), node.stale);
    entry.insert(QStringLiteral("column"), node.column);
    entry.insert(QStringLiteral("version_name"), node.versionName);
    // 版本引用（可追溯）：路径取 catalog 版本记录，不编造。
    entry.insert(QStringLiteral("path"),
                 catalog->versionById(node.versionId).path);
    nodes.append(entry);
  }
  payload.insert(QStringLiteral("nodes"), nodes);
  QJsonArray edges;
  for (const paleo::derivation::Edge &edge : graph.edges) {
    QJsonObject entry;
    entry.insert(QStringLiteral("parent"), edge.parentId);
    entry.insert(QStringLiteral("child"), edge.childId);
    edges.append(entry);
  }
  payload.insert(QStringLiteral("edges"), edges);
  QStringList closureIds = closure.values();
  closureIds.sort(); // QSet 迭代序不稳定：出参排序钉死可复现
  QJsonObject selection;
  selection.insert(QStringLiteral("count"), closureIds.size());
  QJsonArray closureArray;
  for (const QString &id : closureIds)
    closureArray.append(id);
  selection.insert(QStringLiteral("version_ids"), closureArray);
  payload.insert(QStringLiteral("selection"), selection);
  payload.insert(QStringLiteral("collapsed_upstream"), graph.collapsedUpstream);
  payload.insert(QStringLiteral("collapsed_downstream"),
                 graph.collapsedDownstream);
  payload.insert(QStringLiteral("collapsed_seeds"), graph.collapsedSeeds);
  if (!graph.message.isEmpty())
    payload.insert(QStringLiteral("message"), graph.message);
  finishActive(true, payload);
}

QString AiChatToolRunner::projectBrief(const DataCatalog *catalog,
                                       const QString &projectDir) {
  if (!catalog || !catalog->isOpen())
    return tr("未打开——工程数据查询（query/lineage 工具）会得到如实报错。");
  const QHash<QString, int> entities = catalog->entityCountsByType();
  const int wells = entities.value(QStringLiteral("well"));
  const int planned = entities.value(QStringLiteral("planned"));
  const int surveys = entities.value(QStringLiteral("seismic_survey"));
  const int boundaries = entities.value(QStringLiteral("sequence_boundary"));
  const int auxiliary = entities.value(QStringLiteral("auxiliary"));
  QHash<QString, int> assetCounts;
  for (const CatalogAsset &asset : catalog->assets())
    ++assetCounts[asset.type];
  // 计数降序、同数按名稳定排序；超上限归并成「等 N 类」。
  QList<QPair<QString, int>> ranked;
  for (auto it = assetCounts.cbegin(); it != assetCounts.cend(); ++it)
    ranked.append(qMakePair(it.key(), it.value()));
  std::sort(ranked.begin(), ranked.end(),
            [](const QPair<QString, int> &a, const QPair<QString, int> &b) {
              return a.second == b.second ? a.first < b.first
                                          : a.second > b.second;
            });
  QStringList assetParts;
  for (int i = 0; i < ranked.size() && i < kBriefAssetTypes; ++i)
    assetParts.append(tr("%1 %2").arg(ranked[i].first).arg(ranked[i].second));
  if (ranked.size() > kBriefAssetTypes)
    assetParts.append(tr("等 %1 类").arg(ranked.size()));
  QString brief = tr("井 %1 口（其中计划井 %2）、地震测线 %3、层序界面 %4、辅助 %5；"
                     "资产：%6；catalog 修订 %7。")
                    .arg(wells + planned)
                    .arg(planned)
                    .arg(surveys)
                    .arg(boundaries)
                    .arg(auxiliary)
                    .arg(assetParts.isEmpty() ? tr("（无）") : assetParts.join(QStringLiteral("、")))
                    .arg(catalog->catalogRevision());
  if (!projectDir.isEmpty())
    brief = tr("工程 %1：").arg(QFileInfo(projectDir).fileName()) + brief;
  if (brief.size() > kBriefCharCap)
    brief = brief.left(kBriefCharCap - 1) + QStringLiteral("…");
  return brief;
}
