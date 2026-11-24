// 层：功能
#include "agentnavigation.h"
#include "../ai/chat/navigationtools.h"
#include "../catalog/datacatalog.h"

#include <QJsonArray>
#include <QTimer>
#include <algorithm>
#include <cmath>

// workflow/agentnavigation.cpp — I4：导航原子编排实现。
//
// 本层只编排：目标身份用受信 catalog 解析（W2）、动作经三个宿主访问接口
// 委托、异步预览有有限截止（超时=终态，迟到回执按请求代次丢弃，不产生
// 第二终态——W8）。所有结果带 contract=paleo.navigation.v1；错误用稳定
// code，不把模型文本/业务名当可执行内容。

namespace paleo::agent::navigation {
namespace {
// 井资料/测线打开的截止：大外链 SHA 复验 + 索引 + 解码的有界上界；到达即
// 报 timeout（目标画面可能仍在后台完成——那是用户可见事实，不是工具终态）。
constexpr int kOpenDeadlineMs = 60000;
// 结果里井清单回显上限（防大数组回灌模型；完整选择经 navigation_state
// 分页语义本就不承诺全量）。
constexpr int kEchoWellsCap = 16;

QString boundedJoin(const QStringList &ids) {
  QStringList echo;
  for (int i = 0; i < ids.size() && i < kEchoWellsCap; ++i)
    echo.append(ids.at(i));
  if (ids.size() > kEchoWellsCap)
    echo.append(QStringLiteral("…(%1)").arg(ids.size() - kEchoWellsCap));
  return echo.join(QLatin1Char(','));
}

bool wellIdsFromValue(const QJsonValue &value, QStringList *out) {
  if (!value.isArray())
    return false;
  const auto array = value.toArray();
  if (array.isEmpty() || array.size() > kMaxWellIds)
    return false;
  for (const auto &item : array) {
    if (!item.isString())
      return false;
    const QString id = item.toString();
    if (id.isEmpty() || id.size() > kMaxIdLength)
      return false;
    if (!out->contains(id))
      out->append(id); // 去重且保持首次出现顺序（任务书 W1）
  }
  return !out->isEmpty();
}
} // namespace

QJsonObject navigationResult(bool ok, const QString &code, const QJsonObject &facts) {
  QJsonObject payload;
  payload.insert(QStringLiteral("ok"), ok);
  payload.insert(QStringLiteral("contract"), QLatin1String(kContract));
  if (!code.isEmpty())
    payload.insert(QStringLiteral("code"), code);
  for (auto it = facts.constBegin(); it != facts.constEnd(); ++it)
    payload.insert(it.key(), it.value());
  return payload;
}

QString validateNavigationArguments(const QString &name, const QJsonObject &args,
                                    QString *field) {
  if (field)
    field->clear();
  const auto fail = [field](const QString &code, const QString &where) {
    if (field)
      *field = where;
    return code;
  };
  const bool wellIdsPresent = args.contains(QStringLiteral("well_ids"));
  if (name == QLatin1String(kSelectWellsTool)) {
    QStringList ids;
    if (!wellIdsFromValue(args.value(QStringLiteral("well_ids")), &ids))
      return fail(QStringLiteral("invalid_well_ids"), QStringLiteral("well_ids"));
    return QString();
  }
  if (name == QLatin1String(kClearWellSelectionTool)) {
    const auto token = args.value(QStringLiteral("selection_token"));
    if (token.isUndefined())
      return QString();
    if (!token.isString() || token.toString().size() > kMaxTokenLength)
      return fail(QStringLiteral("invalid_selection_token"), QStringLiteral("selection_token"));
    return QString();
  }
  if (name == QLatin1String(kMapLocateTool)) {
    const bool layerPresent = args.contains(QStringLiteral("layer_id"));
    if (wellIdsPresent == layerPresent)
      return fail(QStringLiteral("mutually_exclusive_target"),
                  wellIdsPresent ? QStringLiteral("well_ids") : QStringLiteral("layer_id"));
    if (layerPresent) {
      const auto layer = args.value(QStringLiteral("layer_id"));
      if (!layer.isString() || layer.toString().isEmpty() ||
          layer.toString().size() > kMaxIdLength)
        return fail(QStringLiteral("invalid_layer_id"), QStringLiteral("layer_id"));
    } else {
      QStringList ids;
      if (!wellIdsFromValue(args.value(QStringLiteral("well_ids")), &ids))
        return fail(QStringLiteral("invalid_well_ids"), QStringLiteral("well_ids"));
    }
    return QString();
  }
  if (name == QLatin1String(kOpenWellViewTool)) {
    for (const char *key : {"well_id", "asset_id", "version_id"}) {
      const auto value = args.value(QLatin1String(key));
      if (!value.isString() || value.toString().isEmpty() ||
          value.toString().size() > kMaxIdLength)
        return fail(QStringLiteral("invalid_target_id"), QLatin1String(key));
    }
    return QString();
  }
  if (name == QLatin1String(kOpenSeismicLineTool)) {
    for (const char *key : {"asset_id", "version_id"}) {
      const auto value = args.value(QLatin1String(key));
      if (!value.isString() || value.toString().isEmpty() ||
          value.toString().size() > kMaxIdLength)
        return fail(QStringLiteral("invalid_target_id"), QLatin1String(key));
    }
    const auto axis = args.value(QStringLiteral("axis"));
    if (!axis.isString() || (axis.toString() != QLatin1String("inline") &&
                             axis.toString() != QLatin1String("crossline")))
      return fail(QStringLiteral("invalid_axis"), QStringLiteral("axis"));
    const auto line = args.value(QStringLiteral("line_number"));
    if (!line.isDouble())
      return fail(QStringLiteral("invalid_integer_representation"), QStringLiteral("line_number"));
    const double d = line.toDouble();
    if (d != std::floor(d) || d < kMinLineNumber || d > kMaxLineNumber)
      return fail(QStringLiteral("argument_out_of_range"), QStringLiteral("line_number"));
    return QString();
  }
  // navigation_state：无参（空对象）；未知字段由 AiToolSpec 通用档拒绝。
  return QString();
}

AgentNavigation::AgentNavigation(QObject *parent) : QObject(parent) {}

AgentNavigation::~AgentNavigation() { ++m_generation; }

void AgentNavigation::setSelectionAccess(SelectionAccess *access, QObject *owner) {
  if (access && !owner)
    access = nullptr; // 无 owner 无法守卫寿命：诚实缺位优于悬垂可用性
  m_selection = access;
  m_selectionOwner = owner;
}

void AgentNavigation::setMapAccess(MapAccess *access, QObject *owner) {
  if (access && !owner)
    access = nullptr;
  m_map = access;
  m_mapOwner = owner;
}

void AgentNavigation::setPreviewAccess(PreviewAccess *access, QObject *owner) {
  if (access && !owner)
    access = nullptr;
  m_preview = access;
  m_previewOwner = owner;
}

QString AgentNavigation::unavailabilityReason(const QString &name,
                                              bool *catalogRequired) const {
  if (catalogRequired)
    *catalogRequired = false;
  if (!isNavigationTool(name))
    return QStringLiteral("tool_not_found");
  const bool state = name == QLatin1String(kStateTool);
  const bool selectionFamily = name == QLatin1String(kSelectWellsTool) ||
      name == QLatin1String(kClearWellSelectionTool);
  const bool mapFamily = name == QLatin1String(kMapLocateTool);
  const bool previewFamily = name == QLatin1String(kOpenWellViewTool) ||
      name == QLatin1String(kOpenSeismicLineTool);
  if (previewFamily && (!m_preview || m_previewOwner.isNull()))
    return QStringLiteral("preview_provider_unavailable");
  if (selectionFamily && (!m_selection || m_selectionOwner.isNull()))
    return QStringLiteral("selection_provider_unavailable");
  if (mapFamily && (!m_map || m_mapOwner.isNull()))
    return QStringLiteral("map_provider_unavailable");
  if (state) {
    // 状态查询聚合三个来源；全部缺位才算不可用（部分可用子能力如实列出）。
    const bool anyProvider =
        (m_selection && !m_selectionOwner.isNull()) || (m_map && !m_mapOwner.isNull()) ||
        (m_preview && !m_previewOwner.isNull());
    if (!anyProvider)
      return QStringLiteral("navigation_provider_unavailable");
  }
  if (!state) {
    if (catalogRequired)
      *catalogRequired = true; // 目标解析工具需要打开的工程目录
  }
  return QString();
}

void AgentNavigation::cancel() {
  ++m_generation;
  m_activeSerial = 0;
  m_activeDone = {};
  if (m_deadline)
    m_deadline->stop();
}

void AgentNavigation::startDeadline(int ms) {
  if (ms <= 0)
    return;
  if (!m_deadline) {
    m_deadline = new QTimer(this);
    m_deadline->setSingleShot(true);
    connect(m_deadline, &QTimer::timeout, this, [this]() {
      if (!m_activeSerial || !m_activeDone)
        return;
      // 超时=终态：此后迟到的 done 按代次丢弃（用户画面可能仍在后台完成，
      // 那不是工具终态；已发生的临时改变不被抹掉）。
      const quint64 serial = m_activeSerial;
      const Completion done = m_activeDone;
      m_activeSerial = 0;
      m_activeDone = {};
      if (done)
        done(false, navigationResult(false, QStringLiteral("timeout"),
                                     {{QStringLiteral("detail"),
                                       tr("目标未在期限内确认；页面可能在后台继续完成，"
                                          "稍后用 navigation_state 查询实际状态")}}));
      Q_UNUSED(serial);
    });
  }
  m_deadline->start(ms);
}

void AgentNavigation::finish(quint64 serial, bool ok, const QJsonObject &payload) {
  if (serial != m_activeSerial || !m_activeDone)
    return; // 已超时/被作废/被顶替：迟到回执不产生第二终态
  const Completion done = m_activeDone;
  m_activeSerial = 0;
  m_activeDone = {};
  if (m_deadline)
    m_deadline->stop();
  if (done)
    done(ok, payload);
}

QString AgentNavigation::resolveWellIds(const QStringList &ids, DataCatalog *catalog,
                                        QStringList *resolved) const {
  QStringList unknown;
  for (const QString &id : ids) {
    const CatalogEntity entity = catalog->entityById(id);
    if (entity.id.isEmpty() || entity.entityType != QLatin1String("well")) {
      unknown.append(id);
      continue;
    }
    resolved->append(id);
  }
  if (!unknown.isEmpty())
    return QStringLiteral("unknown_well_ids");
  return QString();
}

QString AgentNavigation::resolveWellViewTarget(const QJsonObject &args,
                                               DataCatalog *catalog, QString *assetId,
                                               QString *versionId, QString *wellId) const {
  *wellId = args.value(QStringLiteral("well_id")).toString();
  *assetId = args.value(QStringLiteral("asset_id")).toString();
  *versionId = args.value(QStringLiteral("version_id")).toString();
  const CatalogAsset asset = catalog->assetById(*assetId);
  if (asset.id.isEmpty())
    return QStringLiteral("unknown_asset");
  if (!isSupportedWellViewAssetType(asset.type))
    return QStringLiteral("unsupported_well_view_type");
  const CatalogVersion version = catalog->versionById(*versionId);
  if (version.id.isEmpty() || version.assetId != *assetId)
    return QStringLiteral("version_not_in_asset");
  // 井关联已决（未决/歧义关联拒绝，不落「全部井」或旧井）。
  bool linked = false;
  for (const EntityAssetLink &l : catalog->linksForAsset(*assetId)) {
    if (l.entityType == QLatin1String("well") && !l.unresolved && !l.entityId.isEmpty() &&
        l.entityId == *wellId) {
      linked = true;
      break;
    }
  }
  if (!linked)
    return QStringLiteral("well_not_linked_to_asset");
  const CatalogEntity well = catalog->entityById(*wellId);
  if (well.id.isEmpty() || well.entityType != QLatin1String("well"))
    return QStringLiteral("unknown_well");
  return QString();
}

QString AgentNavigation::resolveSeismicTarget(const QJsonObject &args,
                                              DataCatalog *catalog, QString *assetId,
                                              QString *versionId, QString *axis,
                                              int *lineNumber) const {
  *assetId = args.value(QStringLiteral("asset_id")).toString();
  *versionId = args.value(QStringLiteral("version_id")).toString();
  *axis = args.value(QStringLiteral("axis")).toString();
  *lineNumber = args.value(QStringLiteral("line_number")).toInt();
  const CatalogAsset asset = catalog->assetById(*assetId);
  if (asset.id.isEmpty())
    return QStringLiteral("unknown_asset");
  if (asset.type != QLatin1String("seismic"))
    return QStringLiteral("not_seismic_asset");
  const CatalogVersion version = catalog->versionById(*versionId);
  if (version.id.isEmpty() || version.assetId != *assetId)
    return QStringLiteral("version_not_in_asset");
  // survey 冻结几何给出有界预检（真实存在性由解码结果判定，不扫描 SEG-Y）。
  QString surveyId;
  for (const EntityAssetLink &l : catalog->linksForAsset(*assetId))
    if (l.role == QLatin1String("seismic_volume") && !l.unresolved && !l.entityId.isEmpty())
      surveyId = l.entityId;
  const CatalogEntity survey =
      surveyId.isEmpty() ? CatalogEntity() : catalog->entityById(surveyId);
  const double lo = *axis == QLatin1String("inline") ? survey.inlineMin : survey.xlineMin;
  const double hi = *axis == QLatin1String("inline") ? survey.inlineMax : survey.xlineMax;
  const bool unbounded = lo == 0.0 && hi == 0.0; // 无 survey 元数据：控件回退域
  const int fallbackLimit = 1000000;
  if (!unbounded && (*lineNumber < lo || *lineNumber > hi))
    return QStringLiteral("line_out_of_survey_range");
  if (unbounded && (*lineNumber < 0 || *lineNumber > fallbackLimit))
    return QStringLiteral("line_out_of_survey_range");
  return QString();
}

void AgentNavigation::run(const QString &name, const QJsonObject &args,
                          const Environment &env, const Completion &done) {
  if (!isNavigationTool(name) || !done) {
    if (done)
      done(false, navigationResult(false, QStringLiteral("tool_not_found")));
    return;
  }
  {
    QString field;
    const QString code = validateNavigationArguments(name, args, &field);
    if (!code.isEmpty()) {
      QJsonObject facts;
      if (!field.isEmpty())
        facts.insert(QStringLiteral("field"), field);
      done(false, navigationResult(false, code, facts));
      return;
    }
  }
  const bool needsCatalog = name != QLatin1String(kStateTool);
  if (needsCatalog && (!env.catalog || !env.catalog->isOpen())) {
    done(false, navigationResult(false, QStringLiteral("catalog_unavailable")));
    return;
  }
  ++m_requestSerial; // 同刻单在途：新请求顶替旧请求（旧 done 不再触发）
  const quint64 serial = m_requestSerial;
  m_activeSerial = serial;
  m_activeDone = done;
  const int generation = m_generation;
  // 审查修复（I4-R5）：回执闭包会被预览页挂起表持有到晚回执——QPointer
  // 守卫编排器寿命，防止其先于预览页析构时晚回执触达悬垂 this。
  const QPointer<AgentNavigation> guard(this);
  const auto wrap = [this, guard, serial, generation](bool ok, const QJsonObject &payload) {
    if (!guard || generation != m_generation)
      return; // 编排器已销毁 / 已被 cancel 作废
    finish(serial, ok, payload);
  };

  if (name == QLatin1String(kStateTool)) {
    QJsonObject document;
    document.insert(QStringLiteral("open"), bool(env.catalog && env.catalog->isOpen()));
    document.insert(QStringLiteral("id"), env.documentId);
    QJsonObject payload;
    payload.insert(QStringLiteral("document"), document);
    if (m_selection && !m_selectionOwner.isNull())
      payload.insert(QStringLiteral("selection"), m_selection->snapshot());
    else
      payload.insert(QStringLiteral("selection"),
                     QJsonObject{{QStringLiteral("available"), false}});
    if (m_map && !m_mapOwner.isNull())
      payload.insert(QStringLiteral("map"), m_map->snapshot());
    else
      payload.insert(QStringLiteral("map"),
                     QJsonObject{{QStringLiteral("available"), false}});
    if (m_preview && !m_previewOwner.isNull())
      payload.insert(QStringLiteral("previews"), m_preview->previewSnapshot());
    else
      payload.insert(QStringLiteral("previews"), QJsonArray());
    wrap(true, navigationResult(true, QString(), payload));
    return;
  }

  if (name == QLatin1String(kSelectWellsTool)) {
    if (!m_selection || m_selectionOwner.isNull()) {
      wrap(false, navigationResult(false, QStringLiteral("selection_provider_unavailable")));
      return;
    }
    QStringList ids;
    wellIdsFromValue(args.value(QStringLiteral("well_ids")), &ids);
    QStringList resolved;
    const QString unknown = resolveWellIds(ids, env.catalog, &resolved);
    if (!unknown.isEmpty()) {
      // 整批拒绝：不留下本批部分选中（选择只在完整解析后替换——W3）。
      wrap(false, navigationResult(false, unknown,
                                   {{QStringLiteral("unknown_wells"), boundedJoin(ids)}}));
      return;
    }
    const QJsonObject facts = m_selection->select(resolved);
    const bool ok = facts.value(QStringLiteral("ok")).toBool(false);
    wrap(ok, navigationResult(ok, ok ? QString() : facts.value(QStringLiteral("code")).toString(), facts));
    return;
  }

  if (name == QLatin1String(kClearWellSelectionTool)) {
    if (!m_selection || m_selectionOwner.isNull()) {
      wrap(false, navigationResult(false, QStringLiteral("selection_provider_unavailable")));
      return;
    }
    const QJsonObject snapshot = m_selection->snapshot();
    const QStringList current = [&snapshot] {
      QStringList ids;
      for (const auto &value : snapshot.value(QStringLiteral("wells")).toArray())
        ids.append(value.toString());
      return ids;
    }();
    const QString token = args.value(QStringLiteral("selection_token")).toString();
    if (!token.isEmpty() && token != snapshot.value(QStringLiteral("token")).toString()) {
      wrap(false, navigationResult(false, QStringLiteral("stale_selection"),
                                   {{QStringLiteral("status"), QStringLiteral("stale_selection")}}));
      return;
    }
    if (current.isEmpty()) {
      wrap(true, navigationResult(true, QString(),
                                  {{QStringLiteral("status"), QStringLiteral("no_change")}}));
      return;
    }
    // 当前共享选择已切到其他域（含非井目标）时不动用户的其他工作。
    bool foreign = false;
    for (const QString &id : current) {
      const CatalogEntity entity = env.catalog->entityById(id);
      if (entity.id.isEmpty() || entity.entityType != QLatin1String("well")) {
        foreign = true;
        break;
      }
    }
    if (foreign) {
      wrap(false, navigationResult(false, QStringLiteral("foreign_selection"),
                                   {{QStringLiteral("status"), QStringLiteral("foreign_selection")}}));
      return;
    }
    const QJsonObject facts = m_selection->clear();
    const bool ok = facts.value(QStringLiteral("ok")).toBool(false);
    wrap(ok, navigationResult(ok, ok ? QString() : facts.value(QStringLiteral("code")).toString(), facts));
    return;
  }

  if (name == QLatin1String(kMapLocateTool)) {
    if (!m_map || m_mapOwner.isNull()) {
      wrap(false, navigationResult(false, QStringLiteral("map_provider_unavailable")));
      return;
    }
    const bool fit = args.value(QStringLiteral("mode")).toString() == QLatin1String("fit");
    QJsonObject facts;
    if (args.contains(QStringLiteral("layer_id"))) {
      facts = m_map->locateLayer(args.value(QStringLiteral("layer_id")).toString(), fit);
    } else {
      QStringList ids;
      wellIdsFromValue(args.value(QStringLiteral("well_ids")), &ids);
      QStringList resolved;
      const QString unknown = resolveWellIds(ids, env.catalog, &resolved);
      if (!unknown.isEmpty()) {
        wrap(false, navigationResult(false, unknown,
                                     {{QStringLiteral("unknown_wells"), boundedJoin(ids)}}));
        return;
      }
      // 坐标来自受信 catalog（工程网格局部米；无坐标井整批拒绝——不伪造
      // 坐标也不静默跳过：W4「无坐标/缺坐标拒绝」）。
      QVector<MapAccess::WellTarget> targets;
      QStringList uncoordinated;
      for (const QString &id : resolved) {
        const CatalogEntity well = env.catalog->entityById(id);
        if (!well.hasSurface || !std::isfinite(well.surfaceX) || !std::isfinite(well.surfaceY)) {
          uncoordinated.append(id);
          continue;
        }
        targets.append({id, well.surfaceX, well.surfaceY});
      }
      if (!uncoordinated.isEmpty()) {
        wrap(false, navigationResult(false, QStringLiteral("well_without_coordinates"),
                                     {{QStringLiteral("wells"), boundedJoin(uncoordinated)}}));
        return;
      }
      facts = m_map->locateWells(targets, fit);
    }
    const bool ok = facts.value(QStringLiteral("ok")).toBool(false);
    wrap(ok, navigationResult(ok, ok ? QString() : facts.value(QStringLiteral("code")).toString(), facts));
    return;
  }

  if (name == QLatin1String(kOpenWellViewTool) || name == QLatin1String(kOpenSeismicLineTool)) {
    if (!m_preview || m_previewOwner.isNull()) {
      wrap(false, navigationResult(false, QStringLiteral("preview_provider_unavailable")));
      return;
    }
    QString assetId, versionId, wellId, axis;
    int lineNumber = 0;
    QString code = name == QLatin1String(kOpenWellViewTool)
                       ? resolveWellViewTarget(args, env.catalog, &assetId, &versionId, &wellId)
                       : resolveSeismicTarget(args, env.catalog, &assetId, &versionId, &axis,
                                              &lineNumber);
    if (!code.isEmpty()) {
      QJsonObject facts{{QStringLiteral("asset_id"), assetId},
                        {QStringLiteral("version_id"), versionId}};
      if (name == QLatin1String(kOpenWellViewTool))
        facts.insert(QStringLiteral("well_id"), wellId);
      else {
        facts.insert(QStringLiteral("axis"), axis);
        facts.insert(QStringLiteral("line_number"), lineNumber);
      }
      wrap(false, navigationResult(false, code, facts));
      return;
    }
    // 异步打开：有限截止；adapter 的 done 恰好回调一次（超时后迟到回执由
    // wrap 的代次判定丢弃）。shown=完整显示；partial=振幅已显示、附属层位
    // 仍在读取——两者都是目标真实上屏，ok=true 且 status 如实区分。
    startDeadline(kOpenDeadlineMs);
    const auto adapt = [wrap](const QJsonObject &facts) {
      const QString status = facts.value(QStringLiteral("status")).toString();
      const bool ok = status == QLatin1String("shown") || status == QLatin1String("partial");
      wrap(ok, navigationResult(ok, ok ? QString()
                                       : facts.value(QStringLiteral("code")).toString(
                                             QStringLiteral("preview_target_not_shown")),
                                facts));
    };
    QJsonObject rejected = name == QLatin1String(kOpenWellViewTool)
                               ? m_preview->openWellView(versionId, wellId, adapt)
                               : m_preview->openSeismicLine(versionId, axis, lineNumber, adapt);
    if (!rejected.isEmpty()) {
      // 同步拒绝：无异步在途，立即终态并停表。
      if (m_deadline)
        m_deadline->stop();
      m_activeSerial = 0;
      m_activeDone = {};
      done(false, navigationResult(false, rejected.value(QStringLiteral("code")).toString(
                                               QStringLiteral("preview_target_not_shown")),
                                   rejected));
      return;
    }
    return;
  }
}

} // namespace paleo::agent::navigation
