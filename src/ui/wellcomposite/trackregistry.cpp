// 层：视图
#include "trackregistry.h"

#include <QMap>

namespace WellComposite
{

// ----------------------------------------------------------------------------
// TrackSpec 序列化
// ----------------------------------------------------------------------------
QVariantMap TrackSpec::toVariantMap() const
{
  QVariantMap m;
  m.insert(QStringLiteral("type"), typeId);
  m.insert(QStringLiteral("title"), title);
  m.insert(QStringLiteral("width"), width);
  m.insert(QStringLiteral("visible"), visible);
  m.insert(QStringLiteral("print"), printIncluded);
  m.insert(QStringLiteral("params"), params);
  return m;
}

TrackSpec TrackSpec::fromVariantMap(const QVariantMap &map)
{
  TrackSpec spec;
  spec.typeId = map.value(QStringLiteral("type")).toString();
  spec.title = map.value(QStringLiteral("title")).toString();
  spec.width = map.value(QStringLiteral("width"), 100.0).toReal();
  spec.visible = map.value(QStringLiteral("visible"), true).toBool();
  spec.printIncluded = map.value(QStringLiteral("print"), true).toBool();
  spec.params = map.value(QStringLiteral("params")).toMap();
  return spec;
}

// ----------------------------------------------------------------------------
// 曲线覆盖应用（D1.7 各曲线独立量程/单位/色 + D3.12 覆盖层）
// ----------------------------------------------------------------------------
CurveData applyCurveOverride(const CurveData &src, const QVariantMap &overrideMap)
{
  CurveData c = src;
  if (overrideMap.isEmpty())
    return c;
  if (overrideMap.contains(QStringLiteral("min")))
    c.minScale = overrideMap.value(QStringLiteral("min")).toFloat();
  if (overrideMap.contains(QStringLiteral("max")))
    c.maxScale = overrideMap.value(QStringLiteral("max")).toFloat();
  if (overrideMap.contains(QStringLiteral("log")))
    c.isLogarithmic = overrideMap.value(QStringLiteral("log")).toBool();
  if (overrideMap.contains(QStringLiteral("unit")))
    c.unit = overrideMap.value(QStringLiteral("unit")).toString();
  if (overrideMap.contains(QStringLiteral("color")))
    c.color = QColor(overrideMap.value(QStringLiteral("color")).toString());
  if (overrideMap.contains(QStringLiteral("penWidth")))
    c.penWidth = overrideMap.value(QStringLiteral("penWidth")).toFloat();
  return c;
}

// ----------------------------------------------------------------------------
// TrackRegistry
// ----------------------------------------------------------------------------
QString TrackRegistry::typeIdForEnum(TrackType type)
{
  switch (type)
  {
  case TrackType::DepthScale: return QStringLiteral("depth");
  case TrackType::Text: return QStringLiteral("text");
  case TrackType::Formation: return QStringLiteral("formation");
  case TrackType::Lithology: return QStringLiteral("litho");
  case TrackType::Core: return QStringLiteral("core");
  case TrackType::Image: return QStringLiteral("image");
  case TrackType::Curve: return QStringLiteral("curve");
  case TrackType::Symbol: return QStringLiteral("symbol");
  case TrackType::StratigraphyCompound: return QStringLiteral("strat");
  case TrackType::FaciesCompound: return QStringLiteral("facies");
  }
  return QString();
}

TrackType TrackRegistry::enumForTypeId(const QString &typeId)
{
  static const QMap<QString, TrackType> map = {
      {QStringLiteral("depth"), TrackType::DepthScale},
      {QStringLiteral("text"), TrackType::Text},
      {QStringLiteral("formation"), TrackType::Formation},
      {QStringLiteral("litho"), TrackType::Lithology},
      {QStringLiteral("core"), TrackType::Core},
      {QStringLiteral("image"), TrackType::Image},
      {QStringLiteral("curve"), TrackType::Curve},
      {QStringLiteral("symbol"), TrackType::Symbol},
      {QStringLiteral("strat"), TrackType::StratigraphyCompound},
      {QStringLiteral("facies"), TrackType::FaciesCompound},
      // curve 道的语义变体（D1.2 词表：Curve/Litho/Facies/Strat/Discrete/Text/Depth/GR）
      {QStringLiteral("gr"), TrackType::Curve},
      {QStringLiteral("discrete"), TrackType::Curve},
  };
  return map.value(typeId, TrackType::Curve);
}

TrackRegistry &TrackRegistry::instance()
{
  static TrackRegistry inst;
  return inst;
}

TrackRegistry::TrackRegistry()
{
  ensureBuiltins();
}

void TrackRegistry::registerType(const TrackRegistryEntry &entry)
{
  m_entries.insert(entry.typeId, entry);
}

bool TrackRegistry::contains(const QString &typeId) const
{
  return m_entries.contains(typeId);
}

const TrackRegistryEntry *TrackRegistry::entry(const QString &typeId) const
{
  const auto it = m_entries.constFind(typeId);
  return it == m_entries.constEnd() ? nullptr : &it.value();
}

QStringList TrackRegistry::typeIds() const
{
  return m_entries.keys();
}

QStringList TrackRegistry::userCreatableTypeIds() const
{
  QStringList ids;
  for (const auto &e : m_entries)
    if (e.userCreatable)
      ids << e.typeId;
  return ids;
}

QString TrackRegistry::displayName(const QString &typeId) const
{
  const auto *e = entry(typeId);
  return e ? e->displayName : typeId;
}

std::shared_ptr<WellTrack> TrackRegistry::createTrack(const TrackSpec &spec) const
{
  const auto *e = entry(spec.typeId);
  if (!e || !e->create)
    return nullptr;
  auto track = e->create(spec);
  if (!track)
    return nullptr;
  track->setTitle(spec.title);
  track->setWidth(spec.width);
  track->setVisible(spec.visible);
  track->setPrintIncluded(spec.printIncluded);
  return track;
}

TrackSpec TrackRegistry::captureSpec(const std::shared_ptr<WellTrack> &track) const
{
  TrackSpec spec;
  if (!track)
    return spec;
  spec.typeId = typeIdForEnum(track->type());
  const auto *e = entry(spec.typeId);
  if (e && e->capture)
    spec = e->capture(track); // 捕获器只填类型参数
  spec.typeId = typeIdForEnum(track->type()); // typeId 统一由此处钉死
  spec.title = track->title();
  spec.width = track->width();
  spec.visible = track->isVisible();
  spec.printIncluded = track->isPrintIncluded();
  return spec;
}

// ----------------------------------------------------------------------------
// 内置类型注册（D1.2 词表全覆盖：Curve/Litho/Facies/Strat/Discrete/Text/Depth/GR
// + 既有 Core/Image/Symbol/Formation）
// ----------------------------------------------------------------------------
void TrackRegistry::ensureBuiltins()
{
  if (!m_entries.isEmpty())
    return;

  // 公共捕获器：标题/宽度/可见性由 captureSpec 统一回填，这里只捕获类型参数
  const auto noParams = [](const std::shared_ptr<WellTrack> &) { return TrackSpec(); };

  registerType({QStringLiteral("depth"), QStringLiteral("深度标尺道"), 64.0, false,
                [](const TrackSpec &spec) -> std::shared_ptr<WellTrack> {
                  auto t = std::make_shared<DepthScaleTrack>(spec.width);
                  const QString ratio = spec.params.value(QStringLiteral("scaleRatio")).toString();
                  if (!ratio.isEmpty())
                    t->setScaleRatio(ratio);
                  return t;
                },
                [](const std::shared_ptr<WellTrack> &t) -> TrackSpec {
                  TrackSpec spec;
                  spec.typeId = QStringLiteral("depth");
                  if (auto dst = std::dynamic_pointer_cast<DepthScaleTrack>(t))
                    spec.params.insert(QStringLiteral("scaleRatio"), dst->scaleRatio());
                  return spec;
                }});

  registerType({QStringLiteral("text"), QStringLiteral("文本道"), 110.0, true,
                [](const TrackSpec &) -> std::shared_ptr<WellTrack> {
                  return std::make_shared<TextTrack>();
                },
                noParams});

  registerType({QStringLiteral("formation"), QStringLiteral("地层分层道"), 80.0, true,
                [](const TrackSpec &) -> std::shared_ptr<WellTrack> {
                  return std::make_shared<FormationTrack>();
                },
                noParams});

  registerType({QStringLiteral("litho"), QStringLiteral("岩性道"), 80.0, true,
                [](const TrackSpec &) -> std::shared_ptr<WellTrack> {
                  return std::make_shared<LithologyTrack>();
                },
                noParams});

  registerType({QStringLiteral("core"), QStringLiteral("取芯道"), 65.0, true,
                [](const TrackSpec &) -> std::shared_ptr<WellTrack> {
                  return std::make_shared<CoreTrack>();
                },
                noParams});

  registerType({QStringLiteral("image"), QStringLiteral("图片道"), 110.0, true,
                [](const TrackSpec &) -> std::shared_ptr<WellTrack> {
                  return std::make_shared<ImageTrack>();
                },
                noParams});

  registerType({QStringLiteral("symbol"), QStringLiteral("符号道"), 48.0, true,
                [](const TrackSpec &) -> std::shared_ptr<WellTrack> {
                  return std::make_shared<SymbolTrack>();
                },
                noParams});

  registerType({QStringLiteral("strat"), QStringLiteral("地层组合道 (系|统|组)"), 145.0, true,
                [](const TrackSpec &) -> std::shared_ptr<WellTrack> {
                  return std::make_shared<StratigraphyCompoundTrack>();
                },
                noParams});

  registerType({QStringLiteral("facies"), QStringLiteral("沉积相组合道 (相|亚|微)"), 180.0, true,
                [](const TrackSpec &) -> std::shared_ptr<WellTrack> {
                  return std::make_shared<FaciesCompoundTrack>();
                },
                noParams});

  // 曲线道族（curve = 通用组合 / gr = 单 GR 预设 / discrete = 离散散点）：
  // 道内曲线集在 spec.params.curves（曲线名列表），数据体由装配方（面板）按名注入。
  const auto makeCurveTrack = [](const TrackSpec &spec, CurveDisplayMode mode) -> std::shared_ptr<WellTrack> {
    auto t = std::make_shared<CurveTrack>(spec.title, spec.width);
    t->setShowGrid(spec.showGrid());
    t->setGridDensity(spec.gridDensity());
    // curves 数据注入走 WellCompositePanel::applySpec 之后（工厂只建空壳，
    // 数据体由 TrackAssembly 在有 ComprehensiveWellData 上下文时回填——见 trackops）
    Q_UNUSED(mode);
    return t;
  };
  registerType({QStringLiteral("curve"), QStringLiteral("测井曲线道"), 170.0, true,
                [makeCurveTrack](const TrackSpec &spec) { return makeCurveTrack(spec, CurveDisplayMode::Continuous); },
                [](const std::shared_ptr<WellTrack> &t) -> TrackSpec {
                  TrackSpec spec;
                  spec.typeId = QStringLiteral("curve");
                  if (auto ct = std::dynamic_pointer_cast<CurveTrack>(t))
                  {
                    QStringList names;
                    for (const auto &c : ct->curves())
                      names << c.name;
                    spec.params.insert(QStringLiteral("curves"), names);
                    spec.params.insert(QStringLiteral("showGrid"), ct->showGrid());
                    spec.params.insert(QStringLiteral("gridDensity"), ct->gridDensity());
                  }
                  return spec;
                }});
  registerType({QStringLiteral("gr"), QStringLiteral("GR 岩性曲线道"), 120.0, true,
                [makeCurveTrack](const TrackSpec &spec) { return makeCurveTrack(spec, CurveDisplayMode::Continuous); },
                [](const std::shared_ptr<WellTrack> &t) -> TrackSpec {
                  TrackSpec spec;
                  spec.typeId = QStringLiteral("gr");
                  if (auto ct = std::dynamic_pointer_cast<CurveTrack>(t))
                  {
                    QStringList names;
                    for (const auto &c : ct->curves())
                      names << c.name;
                    spec.params.insert(QStringLiteral("curves"), names);
                  }
                  return spec;
                }});
  registerType({QStringLiteral("discrete"), QStringLiteral("离散实测散点道"), 160.0, true,
                [makeCurveTrack](const TrackSpec &spec) { return makeCurveTrack(spec, CurveDisplayMode::Discrete); },
                [](const std::shared_ptr<WellTrack> &t) -> TrackSpec {
                  TrackSpec spec;
                  spec.typeId = QStringLiteral("discrete");
                  if (auto ct = std::dynamic_pointer_cast<CurveTrack>(t))
                  {
                    QStringList names;
                    for (const auto &c : ct->curves())
                      names << c.name;
                    spec.params.insert(QStringLiteral("curves"), names);
                  }
                  return spec;
                }});
}

} // namespace WellComposite
