// 层：数据
#include "faultset.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

namespace paleo::fault {

namespace {
// 单行存储的 schema 版本：字段语义变更（重命名/重义）时递增并在
// fromJson 里加迁移分支；纯增字段不递增。
constexpr int kFaultSetFormatVersion = 1;

// 点列平铺为 [x0,y0,x1,y1,…]：QVariant 嵌套列表在 QJson 转换会被拍平
// （Qt6 行为），平铺口径两端一致最稳。
QVariantList pointsToList(const QVector<QPair<double, double>> &points)
{
    QVariantList list;
    list.reserve(points.size() * 2);
    for (const auto &pt : points) {
        list.append(pt.first);
        list.append(pt.second);
    }
    return list;
}

QVector<QPair<double, double>> pointsFromList(const QVariant &v)
{
    QVector<QPair<double, double>> points;
    const QVariantList list = v.toList();
    if (list.size() % 2 != 0)
        return points;
    for (int i = 0; i + 1 < list.size(); i += 2)
        points.append({list.at(i).toDouble(), list.at(i + 1).toDouble()});
    return points;
}
} // namespace

// ---- FaultSectionRef ----

QString FaultSectionRef::matchKey() const
{
    if (kind == Arbitrary)
        return QStringLiteral("arb:%1").arg(pathId);
    return QStringLiteral("idx:%1:%2").arg(kind).arg(index);
}

QVariantMap FaultSectionRef::toMap() const
{
    QVariantMap m;
    m[QStringLiteral("kind")] = kind;
    m[QStringLiteral("index")] = index;
    m[QStringLiteral("pathId")] = pathId;
    m[QStringLiteral("displayName")] = displayName;
    return m;
}

FaultSectionRef FaultSectionRef::fromMap(const QVariantMap &m)
{
    FaultSectionRef ref;
    ref.kind = m.value(QStringLiteral("kind")).toInt();
    if (ref.kind < Inline || ref.kind > Arbitrary)
        ref.kind = Inline;
    ref.index = m.value(QStringLiteral("index")).toInt();
    ref.pathId = m.value(QStringLiteral("pathId")).toString();
    ref.displayName = m.value(QStringLiteral("displayName")).toString();
    return ref;
}

// ---- FaultStick ----

double FaultStick::twtMinMs() const
{
    double v = 0.0;
    bool first = true;
    for (const auto &pt : points) {
        if (first || pt.second < v) {
            v = pt.second;
            first = false;
        }
    }
    return v;
}

double FaultStick::twtMaxMs() const
{
    double v = 0.0;
    bool first = true;
    for (const auto &pt : points) {
        if (first || pt.second > v) {
            v = pt.second;
            first = false;
        }
    }
    return v;
}

QVariantMap FaultStick::toMap() const
{
    QVariantMap m;
    m[QStringLiteral("id")] = id;
    m[QStringLiteral("section")] = section.toMap();
    m[QStringLiteral("points")] = pointsToList(points);
    m[QStringLiteral("interpreter")] = interpreter;
    return m;
}

FaultStick FaultStick::fromMap(const QVariantMap &m)
{
    FaultStick stick;
    stick.id = m.value(QStringLiteral("id")).toString();
    stick.section = FaultSectionRef::fromMap(m.value(QStringLiteral("section")).toMap());
    stick.points = pointsFromList(m.value(QStringLiteral("points")));
    stick.interpreter = m.value(QStringLiteral("interpreter")).toString();
    return stick;
}

bool FaultStick::operator==(const FaultStick &o) const
{
    return id == o.id && section.matchKey() == o.section.matchKey() &&
           section.displayName == o.section.displayName && points == o.points &&
           interpreter == o.interpreter;
}

// ---- FaultHorizonCut ----

QVariantMap FaultHorizonCut::toMap() const
{
    QVariantMap m;
    m[QStringLiteral("horizon")] = horizon;
    m[QStringLiteral("wkt")] = wkt;
    m[QStringLiteral("hangingSide")] = hangingSideToString(hangingSide);
    if (!extra.isEmpty())
        m[QStringLiteral("extra")] = extra;
    return m;
}

FaultHorizonCut FaultHorizonCut::fromMap(const QVariantMap &m)
{
    FaultHorizonCut cut;
    cut.horizon = m.value(QStringLiteral("horizon")).toString();
    cut.wkt = m.value(QStringLiteral("wkt")).toString();
    cut.hangingSide = hangingSideFromString(m.value(QStringLiteral("hangingSide")).toString());
    cut.extra = m.value(QStringLiteral("extra")).toMap();
    return cut;
}

bool FaultHorizonCut::operator==(const FaultHorizonCut &o) const
{
    return horizon == o.horizon && wkt == o.wkt && hangingSide == o.hangingSide && extra == o.extra;
}

QString FaultHorizonCut::hangingSideToString(FaultHangingSide side)
{
    switch (side) {
    case FaultHangingSide::Left:
        return QStringLiteral("left");
    case FaultHangingSide::Right:
        return QStringLiteral("right");
    default:
        return QStringLiteral("unknown");
    }
}

FaultHangingSide FaultHorizonCut::hangingSideFromString(const QString &s)
{
    if (s == QLatin1String("left"))
        return FaultHangingSide::Left;
    if (s == QLatin1String("right"))
        return FaultHangingSide::Right;
    return FaultHangingSide::Unknown;
}

// ---- Fault ----

const FaultStick *Fault::stickById(const QString &stickId) const
{
    for (const FaultStick &s : sticks)
        if (s.id == stickId)
            return &s;
    return nullptr;
}

QVariantMap Fault::toMap() const
{
    QVariantMap m;
    m[QStringLiteral("id")] = id;
    m[QStringLiteral("name")] = name;
    m[QStringLiteral("visible")] = visible;
    m[QStringLiteral("interpreter")] = interpreter;
    QVariantList stickList;
    stickList.reserve(sticks.size());
    for (const FaultStick &s : sticks)
        stickList.append(s.toMap());
    m[QStringLiteral("sticks")] = stickList;
    QVariantList cutList;
    cutList.reserve(cuts.size());
    for (const FaultHorizonCut &c : cuts)
        cutList.append(c.toMap());
    m[QStringLiteral("cuts")] = cutList;
    if (!extra.isEmpty())
        m[QStringLiteral("extra")] = extra;
    return m;
}

Fault Fault::fromMap(const QVariantMap &m)
{
    Fault f;
    f.id = m.value(QStringLiteral("id")).toString();
    f.name = m.value(QStringLiteral("name")).toString();
    f.visible = m.value(QStringLiteral("visible"), true).toBool();
    f.interpreter = m.value(QStringLiteral("interpreter")).toString();
    const QVariantList stickList = m.value(QStringLiteral("sticks")).toList();
    for (const QVariant &v : stickList)
        f.sticks.append(FaultStick::fromMap(v.toMap()));
    const QVariantList cutList = m.value(QStringLiteral("cuts")).toList();
    for (const QVariant &v : cutList)
        f.cuts.append(FaultHorizonCut::fromMap(v.toMap()));
    f.extra = m.value(QStringLiteral("extra")).toMap();
    return f;
}

// ---- FaultSet ----

QString FaultSet::addFault(const QString &name, const QString &interpreter)
{
    Fault f;
    f.id = QStringLiteral("f-%1").arg(m_nextFaultId++);
    f.name = uniqueName(name.isEmpty() ? QStringLiteral("F") : name);
    f.interpreter = interpreter;
    f.visible = true;
    m_faults.append(f);
    return f.id;
}

bool FaultSet::removeFault(const QString &faultId)
{
    for (int i = 0; i < m_faults.size(); ++i) {
        if (m_faults[i].id == faultId) {
            m_faults.removeAt(i);
            return true;
        }
    }
    return false;
}

bool FaultSet::renameFault(const QString &faultId, const QString &newName)
{
    Fault *target = nullptr;
    for (Fault &f : m_faults) {
        if (f.id == faultId)
            target = &f;
        else if (f.name == newName)
            return false; // 撞名拒绝
    }
    if (!target)
        return false;
    target->name = newName;
    return true;
}

bool FaultSet::setFaultVisible(const QString &faultId, bool visible)
{
    for (Fault &f : m_faults) {
        if (f.id == faultId) {
            f.visible = visible;
            return true;
        }
    }
    return false;
}

void FaultSet::clear()
{
    m_faults.clear();
    m_nextFaultId = 1;
    m_nextStickId = 1;
}

const Fault *FaultSet::faultById(const QString &faultId) const
{
    for (const Fault &f : m_faults)
        if (f.id == faultId)
            return &f;
    return nullptr;
}

QString FaultSet::uniqueName(const QString &base) const
{
    auto taken = [this](const QString &n) {
        for (const Fault &f : m_faults)
            if (f.name == n)
                return true;
        return false;
    };
    if (!taken(base))
        return base;
    for (int i = 1; i < 1000000; ++i) {
        const QString candidate = QStringLiteral("%1%2").arg(base).arg(i + 1);
        if (!taken(candidate))
            return candidate;
    }
    return QStringLiteral("%1_%2").arg(base).arg(m_nextFaultId);
}

bool FaultSet::hasFaultNamed(const QString &name) const
{
    for (const Fault &f : m_faults)
        if (f.name == name)
            return true;
    return false;
}

void FaultSet::insertFault(const Fault &fault)
{
    if (fault.id.startsWith(QLatin1String("f-")))
        m_nextFaultId = qMax(m_nextFaultId, fault.id.mid(2).toInt() + 1);
    for (const FaultStick &s : fault.sticks)
        if (s.id.startsWith(QLatin1String("s-")))
            m_nextStickId = qMax(m_nextStickId, s.id.mid(2).toInt() + 1);
    m_faults.append(fault);
}

bool FaultSet::addStick(const QString &faultId, const FaultStick &stick, QString *assignedId)
{
    for (Fault &f : m_faults) {
        if (f.id != faultId)
            continue;
        FaultStick s = stick;
        if (s.id.isEmpty()) {
            s.id = QStringLiteral("s-%1").arg(m_nextStickId++);
        } else if (s.id.startsWith(QLatin1String("s-"))) {
            // 外部带来的 s-<纯数字> id：计数器上抬防复用；其他形状原样保留
            const QString suffix = s.id.mid(2);
            bool digits = !suffix.isEmpty();
            for (const QChar &ch : suffix)
                if (!ch.isDigit()) {
                    digits = false;
                    break;
                }
            if (digits)
                m_nextStickId = qMax(m_nextStickId, suffix.toInt() + 1);
        }
        f.sticks.append(s);
        if (assignedId)
            *assignedId = s.id;
        return true;
    }
    return false;
}

bool FaultSet::removeStick(const QString &faultId, const QString &stickId)
{
    for (Fault &f : m_faults) {
        if (f.id != faultId)
            continue;
        for (int i = 0; i < f.sticks.size(); ++i) {
            if (f.sticks[i].id == stickId) {
                f.sticks.removeAt(i);
                return true;
            }
        }
    }
    return false;
}

QVector<QPair<QString, FaultStick>> FaultSet::sticksForSection(const FaultSectionRef &section) const
{
    QVector<QPair<QString, FaultStick>> out;
    const QString key = section.matchKey();
    for (const Fault &f : m_faults) {
        if (!f.visible)
            continue;
        for (const FaultStick &s : f.sticks)
            if (s.section.matchKey() == key)
                out.append({f.id, s});
    }
    return out;
}

bool FaultSet::setCut(const QString &faultId, const FaultHorizonCut &cut)
{
    if (cut.horizon.isEmpty())
        return false;
    for (Fault &f : m_faults) {
        if (f.id != faultId)
            continue;
        for (FaultHorizonCut &c : f.cuts) {
            if (c.horizon == cut.horizon) {
                c = cut; // 同层位替换
                return true;
            }
        }
        f.cuts.append(cut);
        return true;
    }
    return false;
}

bool FaultSet::removeCut(const QString &faultId, const QString &horizon)
{
    for (Fault &f : m_faults) {
        if (f.id != faultId)
            continue;
        for (int i = 0; i < f.cuts.size(); ++i) {
            if (f.cuts[i].horizon == horizon) {
                f.cuts.removeAt(i);
                return true;
            }
        }
    }
    return false;
}

const FaultHorizonCut *FaultSet::cut(const QString &faultId, const QString &horizon) const
{
    const Fault *f = faultById(faultId);
    if (!f)
        return nullptr;
    for (const FaultHorizonCut &c : f->cuts)
        if (c.horizon == horizon)
            return &c;
    return nullptr;
}

QByteArray FaultSet::toJson() const
{
    QJsonObject root;
    root[QStringLiteral("format")] = QStringLiteral("paleo-faultset");
    root[QStringLiteral("version")] = kFaultSetFormatVersion;
    QJsonArray faultArray;
    for (const Fault &f : m_faults)
        faultArray.append(QJsonObject::fromVariantMap(f.toMap()));
    root[QStringLiteral("faults")] = faultArray;
    root[QStringLiteral("nextFaultId")] = m_nextFaultId;
    root[QStringLiteral("nextStickId")] = m_nextStickId;
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool FaultSet::fromJson(const QByteArray &json, QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = QStringLiteral("FaultSet JSON 解析失败: %1").arg(parseError.errorString());
        return false;
    }
    const QJsonObject root = doc.object();
    const QJsonValue faultValue = root.value(QStringLiteral("faults"));
    if (!faultValue.isArray()) {
        if (error)
            *error = QStringLiteral("FaultSet JSON 缺 faults 数组");
        return false;
    }
    QVector<Fault> parsed;
    int maxFaultId = 0, maxStickId = 0;
    for (const QJsonValue &v : faultValue.toArray()) {
        if (!v.isObject()) {
            if (error)
                *error = QStringLiteral("FaultSet faults 元素非对象");
            return false;
        }
        const Fault f = Fault::fromMap(v.toObject().toVariantMap());
        if (f.id.isEmpty() || f.name.isEmpty()) {
            if (error)
                *error = QStringLiteral("FaultSet 断层缺 id/name");
            return false;
        }
        if (f.id.startsWith(QLatin1String("f-")))
            maxFaultId = qMax(maxFaultId, f.id.mid(2).toInt());
        for (const FaultStick &s : f.sticks)
            if (s.id.startsWith(QLatin1String("s-")))
                maxStickId = qMax(maxStickId, s.id.mid(2).toInt());
        parsed.append(f);
    }
    m_faults = parsed;
    m_nextFaultId = qMax(root.value(QStringLiteral("nextFaultId")).toInt(), maxFaultId + 1);
    m_nextStickId = qMax(root.value(QStringLiteral("nextStickId")).toInt(), maxStickId + 1);
    return true;
}

} // namespace paleo::fault
