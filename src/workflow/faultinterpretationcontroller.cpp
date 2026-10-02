// 层：功能
#include "faultinterpretationcontroller.h"

#include "../catalog/datacatalog.h"
#include "../linkage/selectioncontext.h"
#include "../metadata/faultsetstore.h"

#include <QPointer>

#include <functional>

#include <qgsfeature.h>
#include <qgsgeometry.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>

namespace paleo::fault {

namespace {
// 撤销自动新建断层时的收尾：该断层若已无任何内容则一并移除（否则保留
// 其他棒/切割）。
void dropFaultIfEmpty(FaultSet &set, const QString &faultId)
{
    const Fault *f = set.faultById(faultId);
    if (f && f->sticks.isEmpty() && f->cuts.isEmpty())
        set.removeFault(faultId);
}
} // namespace

// ---- FaultEditStack ----

FaultEditStack::FaultEditStack(QObject *parent)
    : QObject(parent)
{
}

void FaultEditStack::push(const QString &text, std::function<void()> undoFn,
                          std::function<void()> redoFn)
{
    truncateRedo();
    Entry entry{text, std::move(undoFn), std::move(redoFn)};
    entry.redoFn(); // push 即执行
    m_done.append(std::move(entry));
    emit canUndoChanged(canUndo());
    emit canRedoChanged(canRedo());
}

void FaultEditStack::undo()
{
    if (m_done.isEmpty())
        return;
    Entry entry = m_done.takeLast();
    entry.undoFn();
    mUndone.append(std::move(entry));
    emit canUndoChanged(canUndo());
    emit canRedoChanged(canRedo());
}

void FaultEditStack::redo()
{
    if (mUndone.isEmpty())
        return;
    Entry entry = mUndone.takeLast();
    entry.redoFn();
    m_done.append(std::move(entry));
    emit canUndoChanged(canUndo());
    emit canRedoChanged(canRedo());
}

void FaultEditStack::clear()
{
    const bool hadUndo = !m_done.isEmpty();
    const bool hadRedo = !mUndone.isEmpty();
    m_done.clear();
    mUndone.clear();
    if (hadUndo)
        emit canUndoChanged(false);
    if (hadRedo)
        emit canRedoChanged(false);
}

void FaultEditStack::truncateRedo()
{
    mUndone.clear();
}

FaultInterpretationController::FaultInterpretationController(FaultSetStore *store,
                                                             SelectionContext *selection,
                                                             QObject *parent)
    : QObject(parent), m_store(store), m_selection(selection)
{
    m_editStack = new FaultEditStack(this);
    if (m_selection)
        connect(m_selection, &SelectionContext::selectionChanged, this,
                [this](const QStringList &ids, const QString &) { onContextSelection(ids, {}); });
}

FaultInterpretationController::~FaultInterpretationController() = default;

bool FaultInterpretationController::reload(QString *error)
{
    m_set.clear();
    if (!m_store->load(m_set, error))
        return false;
    if (!m_activeFaultId.isEmpty() && !m_set.faultById(m_activeFaultId))
        m_activeFaultId.clear();
    refreshMapLayer();
    emit faultSetChanged();
    return true;
}

QString FaultInterpretationController::activeHorizon() const
{
    if (m_selection) {
        const QString h = m_selection->activeHorizon();
        if (!h.isEmpty())
            return h;
    }
    return QStringLiteral("H1");
}

// ---- 编辑原语 ----

QString FaultInterpretationController::addFault(const QString &name,
                                                const QString &interpreter)
{
    const QString faultId = QStringLiteral("f-%1").arg(m_set.nextFaultId());
    const QString unique = m_set.uniqueName(name.isEmpty() ? QStringLiteral("F") : name);
    m_editStack->push(
        tr("新建断层 %1").arg(unique),
        [this, faultId] {
            m_set.removeFault(faultId);
            commitModel();
        },
        [this, faultId, unique, interpreter] {
            Fault f;
            f.id = faultId;
            f.name = unique;
            f.interpreter = interpreter;
            f.visible = true;
            m_set.insertFault(f);
            commitModel();
        });
    m_activeFaultId = faultId; // 新建即拾取目标
    return faultId;
}

QPair<QString, QString> FaultInterpretationController::addStick(const FaultStick &stickIn)
{
    if (stickIn.points.size() < 2)
        return {};

    QString faultId = m_activeFaultId;
    if (!faultId.isEmpty() && !m_set.faultById(faultId))
        faultId.clear(); // 活动断层已被删 → 自动新建
    const bool createFault = faultId.isEmpty();
    QString name;
    if (createFault) {
        faultId = QStringLiteral("f-%1").arg(m_set.nextFaultId());
        name = m_set.uniqueName(QStringLiteral("F"));
    }
    FaultStick stick = stickIn;
    if (stick.id.isEmpty())
        stick.id = QStringLiteral("s-%1").arg(m_set.nextStickId());
    const QString interpreter = stick.interpreter;

    m_editStack->push(
        tr("拾取断层棒 %1").arg(stick.id),
        [this, faultId, stick, createFault] {
            m_set.removeStick(faultId, stick.id);
            if (createFault)
                dropFaultIfEmpty(m_set, faultId);
            commitModel();
        },
        [this, faultId, name, interpreter, stick, createFault] {
            if (createFault && !m_set.faultById(faultId)) {
                Fault f;
                f.id = faultId;
                f.name = name;
                f.interpreter = interpreter;
                f.visible = true;
                m_set.insertFault(f);
            }
            m_set.addStick(faultId, stick, nullptr);
            commitModel();
        });
    if (createFault)
        m_activeFaultId = faultId; // 后续拾取落同一断层
    return {faultId, stick.id};
}

bool FaultInterpretationController::removeStick(const QString &faultId, const QString &stickId)
{
    const Fault *f = m_set.faultById(faultId);
    const FaultStick *s = f ? f->stickById(stickId) : nullptr;
    if (!s)
        return false;
    const FaultStick snapshot = *s;
    m_editStack->push(
        tr("删除断层棒 %1").arg(stickId),
        [this, faultId, snapshot] {
            m_set.addStick(faultId, snapshot, nullptr);
            commitModel();
        },
        [this, faultId, stickId] {
            m_set.removeStick(faultId, stickId);
            commitModel();
        });
    return true;
}

bool FaultInterpretationController::removeFault(const QString &faultId)
{
    const Fault *f = m_set.faultById(faultId);
    if (!f)
        return false;
    const Fault snapshot = *f;
    m_editStack->push(
        tr("删除断层 %1").arg(f->name),
        [this, snapshot] {
            m_set.insertFault(snapshot);
            commitModel();
        },
        [this, faultId] {
            m_set.removeFault(faultId);
            if (m_activeFaultId == faultId)
                m_activeFaultId.clear();
            commitModel();
        });
    return true;
}

bool FaultInterpretationController::renameFault(const QString &faultId, const QString &newName)
{
    const Fault *f = m_set.faultById(faultId);
    if (!f || newName.isEmpty() || f->name == newName)
        return false;
    if (m_set.hasFaultNamed(newName))
        return false; // 撞名预检：不产生空命令（模型层同样拒绝，此处防静默失败）
    const QString oldName = f->name;
    m_editStack->push(
        tr("断层改名 %1→%2").arg(oldName, newName),
        [this, faultId, oldName] {
            m_set.renameFault(faultId, oldName);
            commitModel();
        },
        [this, faultId, newName] {
            m_set.renameFault(faultId, newName);
            commitModel();
        });
    return true;
}

bool FaultInterpretationController::setFaultVisible(const QString &faultId, bool visible)
{
    const Fault *f = m_set.faultById(faultId);
    if (!f || f->visible == visible)
        return false;
    m_editStack->push(
        visible ? tr("显示断层 %1").arg(f->name) : tr("隐藏断层 %1").arg(f->name),
        [this, faultId, visible] {
            m_set.setFaultVisible(faultId, !visible);
            commitModel();
        },
        [this, faultId, visible] {
            m_set.setFaultVisible(faultId, visible);
            commitModel();
        });
    return true;
}

bool FaultInterpretationController::setCut(const QString &faultIdIn, const FaultHorizonCut &cutIn)
{
    FaultHorizonCut cut = cutIn;
    if (cut.horizon.isEmpty())
        cut.horizon = activeHorizon();
    if (cut.horizon.isEmpty() || cut.wkt.isEmpty())
        return false;

    QString faultId = faultIdIn.isEmpty() ? m_activeFaultId : faultIdIn;
    if (!faultId.isEmpty() && !m_set.faultById(faultId))
        faultId.clear();
    const bool createFault = faultId.isEmpty();
    QString name;
    if (createFault) {
        faultId = QStringLiteral("f-%1").arg(m_set.nextFaultId());
        name = m_set.uniqueName(QStringLiteral("F"));
    }

    const FaultHorizonCut *old = m_set.cut(faultId, cut.horizon);
    const bool hadOld = old != nullptr;
    const FaultHorizonCut oldCut = hadOld ? *old : FaultHorizonCut{};

    m_editStack->push(
        tr("断层 %1 层位 %2 切割多边形")
            .arg(createFault ? name : m_set.faultById(faultId)->name, cut.horizon),
        [this, faultId, cut, hadOld, oldCut, createFault] {
            if (hadOld)
                m_set.setCut(faultId, oldCut);
            else
                m_set.removeCut(faultId, cut.horizon);
            if (createFault)
                dropFaultIfEmpty(m_set, faultId);
            commitModel();
        },
        [this, faultId, name, cut, createFault] {
            if (createFault && !m_set.faultById(faultId)) {
                Fault f;
                f.id = faultId;
                f.name = name;
                f.visible = true;
                m_set.insertFault(f);
            }
            m_set.setCut(faultId, cut);
            commitModel();
        });
    return true;
}

bool FaultInterpretationController::removeCut(const QString &faultId, const QString &horizon)
{
    const FaultHorizonCut *c = m_set.cut(faultId, horizon);
    if (!c)
        return false;
    const FaultHorizonCut snapshot = *c;
    m_editStack->push(
        tr("删除 %1 层位切割").arg(horizon),
        [this, faultId, snapshot] {
            m_set.setCut(faultId, snapshot);
            commitModel();
        },
        [this, faultId, horizon] {
            m_set.removeCut(faultId, horizon);
            commitModel();
        });
    return true;
}

bool FaultInterpretationController::setCutHangingSide(const QString &faultId,
                                                      const QString &horizon,
                                                      FaultHangingSide side)
{
    const FaultHorizonCut *c = m_set.cut(faultId, horizon);
    if (!c || c->hangingSide == side)
        return false;
    const FaultHorizonCut oldCut = *c;
    FaultHorizonCut newCut = *c;
    newCut.hangingSide = side;
    m_editStack->push(
        tr("%1 层位上盘方向 → %2").arg(horizon, FaultHorizonCut::hangingSideToString(side)),
        [this, faultId, oldCut] {
            m_set.setCut(faultId, oldCut);
            commitModel();
        },
        [this, faultId, newCut] {
            m_set.setCut(faultId, newCut);
            commitModel();
        });
    return true;
}

// ---- 联动 ----

QString FaultInterpretationController::selectionId(const QString &faultId)
{
    return QStringLiteral("fault:") + faultId;
}

QString FaultInterpretationController::faultIdFromSelectionId(const QString &selectionId)
{
    return selectionId.startsWith(QLatin1String("fault:"))
        ? selectionId.mid(QString("fault:").size())
        : QString();
}

void FaultInterpretationController::selectFaults(const QStringList &faultIds,
                                                 const QString &origin)
{
    QStringList payload;
    for (const QString &id : faultIds) {
        if (m_set.faultById(id))
            payload.append(selectionId(id));
    }
    if (m_selection)
        m_selection->setSelection(payload, origin);
    else
        onContextSelection(payload, origin); // 无联动壳（测试）：本地回声
}

void FaultInterpretationController::onContextSelection(const QStringList &ids, const QString &)
{
    QStringList faultIds;
    for (const QString &id : ids) {
        const QString f = faultIdFromSelectionId(id);
        if (!f.isEmpty() && m_set.faultById(f))
            faultIds.append(f);
    }
    m_selectedFaultIds = faultIds;

    // 地图镜像层回声（防自环：置选时短路边路信号）
    if (m_mapLayer) {
        m_mapLayer->blockSignals(true);
        QgsFeatureIds fids;
        if (m_mapLayer->isValid()) {
            QgsFeatureIterator it = m_mapLayer->getFeatures();
            QgsFeature feat;
            while (it.nextFeature(feat))
                if (faultIds.contains(feat.attribute(QStringLiteral("fault_id")).toString()))
                    fids.insert(feat.id());
        }
        m_mapLayer->selectByIds(fids);
        m_mapLayer->blockSignals(false);
    }
    emit faultSelectionChanged(faultIds);
}

void FaultInterpretationController::onMapLayerSelectionChanged()
{
    if (!m_mapLayer || !m_mapLayer->isValid())
        return;
    QStringList faultIds;
    const QgsFeatureIds selected = m_mapLayer->selectedFeatureIds();
    if (!selected.isEmpty()) {
        QgsFeatureIterator it = m_mapLayer->getSelectedFeatures();
        QgsFeature feat;
        while (it.nextFeature(feat)) {
            const QString id = feat.attribute(QStringLiteral("fault_id")).toString();
            if (!faultIds.contains(id))
                faultIds.append(id);
        }
    }
    selectFaults(faultIds, QStringLiteral("fault_map"));
}

// ---- catalog 角色 ----

void FaultInterpretationController::setCatalogContext(DataCatalog *catalog,
                                                      const QString &entityId,
                                                      const QString &assetId)
{
    m_catalog = catalog;
    m_catalogEntityId = entityId;
    m_catalogAssetId = assetId;
    ensureFaultRoleLink();
}

void FaultInterpretationController::ensureFaultRoleLink()
{
    if (m_faultRoleLinked || !m_catalog || m_catalogEntityId.isEmpty() ||
        m_catalogAssetId.isEmpty() || m_set.faultCount() == 0)
        return;
    for (const EntityAssetLink &l : m_catalog->linksForAsset(m_catalogAssetId)) {
        if (l.entityId == m_catalogEntityId && l.role == QLatin1String("fault") &&
            !l.unresolved) {
            m_faultRoleLinked = true;
            return;
        }
    }
    EntityAssetLink link;
    link.entityType = QStringLiteral("seismic_survey");
    link.entityId = m_catalogEntityId;
    link.assetId = m_catalogAssetId;
    link.role = QStringLiteral("fault"); // 词表既有角色（roleregistry.cpp）
    link.isPrimary = false;
    link.note = QStringLiteral("断层解释（FaultSet）");
    QString err;
    if (m_catalog->addLink(link, &err))
        m_faultRoleLinked = true;
}

// ---- 层位图切割镜像层 ----

QgsVectorLayer *FaultInterpretationController::mapLayer() const
{
    return m_mapLayer;
}

QgsVectorLayer *FaultInterpretationController::ensureMapLayer(const QString &crsAuthId)
{
    if (m_mapLayer)
        return m_mapLayer;
    const QString uri = QStringLiteral(
        "Polygon?crs=%1&field=fault_id:string&field=fault_name:string&field=horizon:string")
                             .arg(crsAuthId);
    QgsVectorLayer *layer =
        new QgsVectorLayer(uri, QStringLiteral("断层切割"), QStringLiteral("memory"));
    if (!layer->isValid()) {
        delete layer;
        return nullptr;
    }
    m_mapLayer = layer;
    QgsProject::instance()->addMapLayer(m_mapLayer);
    connect(m_mapLayer, &QgsVectorLayer::selectionChanged, this,
            [this] { onMapLayerSelectionChanged(); });
    refreshMapLayer();
    return m_mapLayer;
}

void FaultInterpretationController::refreshMapLayer()
{
    if (!m_mapLayer || !m_mapLayer->isValid())
        return;
    QgsVectorLayer *layer = m_mapLayer;
    layer->blockSignals(true);
    layer->selectByIds(QgsFeatureIds());
    layer->dataProvider()->truncate();
    QgsFeatureList features;
    for (const Fault &f : m_set.faults()) {
        if (!f.visible)
            continue;
        for (const FaultHorizonCut &c : f.cuts) {
            QgsFeature feat(layer->fields());
            const QgsGeometry geom = QgsGeometry::fromWkt(c.wkt);
            if (geom.isNull() || geom.isEmpty()) // WKT 解析失败/空几何不入层
                continue;
            feat.setGeometry(geom);
            feat.setAttribute(QStringLiteral("fault_id"), f.id);
            feat.setAttribute(QStringLiteral("fault_name"), f.name);
            feat.setAttribute(QStringLiteral("horizon"), c.horizon);
            features.append(feat);
        }
    }
    layer->dataProvider()->addFeatures(features);
    layer->updateExtents();
    layer->blockSignals(false);
    layer->triggerRepaint();
}

// ---- 内部 ----

bool FaultInterpretationController::commitModel()
{
    QString err;
    const bool ok = m_store->save(m_set, &err);
    m_lastError = ok ? QString() : err;
    ensureFaultRoleLink();
    refreshMapLayer();
    emit faultSetChanged();
    return ok;
}

} // namespace paleo::fault
