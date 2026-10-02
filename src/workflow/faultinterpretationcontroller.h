// 层：功能
#pragma once
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>

#include "../domain/faultset.h"

class FaultSetStore;
class SelectionContext;
class DataCatalog;
class QgsVectorLayer;

namespace paleo::fault {

// 轻量命令栈（闭包式，WellComposite EditStack 同思路）：QUndoStack 属
// QtWidgets 词表禁项，功能层不可用；撤销语义留在编排层，纯 QtCore。
// push 即执行 redo；undo/redo 对称重放；clear 清史（重开工程）。
class FaultEditStack : public QObject
{
    Q_OBJECT
public:
    explicit FaultEditStack(QObject *parent = nullptr);

    void push(const QString &text, std::function<void()> undoFn,
              std::function<void()> redoFn);
    void undo();
    void redo();
    bool canUndo() const { return !m_done.isEmpty(); }
    bool canRedo() const { return !mUndone.isEmpty(); }
    int count() const { return static_cast<int>(m_done.size()); }
    void clear();

signals:
    void canUndoChanged(bool can);
    void canRedoChanged(bool can);

private:
    struct Entry
    {
        QString text;
        std::function<void()> undoFn;
        std::function<void()> redoFn;
    };
    void truncateRedo(); // 新命令作废被撤销的历史
    QVector<Entry> m_done;
    QVector<Entry> mUndone;
};

// workflow/faultinterpretationcontroller — 断层解释编排
// （goal/fault-interpretation）。
//
// 职责（视图只发信号，本类只编排不画像素）：
//   · 唯一权威 FaultSet 模型 + FaultSetStore 落盘（写经工程写队列）；
//   · 编辑原语（建断层/拾棒/删/改名/显隐/层位切割/上盘方向）全部经
//     FaultEditStack 命令——拾取/删除/改名 undo/redo 逐拍可断言（Oracle #2）；
//   · SelectionContext 联动：断层 id 空间 "fault:<id>"，协议形状零改动
//     （Oracle #3）；消费 selectionChanged 剥前缀回声给各视图；
//   · 层位图切割镜像层：内存 Polygon 图层（fault_id/fault_name/horizon），
//     模型变更整刷；图层选中 ⇄ SelectionContext 双向；
//   · catalog 角色：FaultSet 非空时确保 seismic_survey 实体挂 "fault"
//     角色链接（词表既有角色，roleregistry.cpp）。
//
// 边界（Oracle #6）：不做断层封闭性/断距计算；不自动识别断层。
class FaultInterpretationController : public QObject
{
    Q_OBJECT
public:
    // store 须已 open()；selection 可空（无联动壳的测试）。
    FaultInterpretationController(FaultSetStore *store, SelectionContext *selection,
                                  QObject *parent = nullptr);
    ~FaultInterpretationController() override;

    // 工程打开/重开：从 store 读回（空工程 → 空集）。
    bool reload(QString *error = nullptr);

    const FaultSet &faultSet() const { return m_set; }
    FaultEditStack *editStack() const { return m_editStack; }
    // 最近一次落盘失败原因（模型仍已变更；空串 = 无失败）。
    QString lastError() const { return m_lastError; }

    // ---- 活动上下文 ----
    // 剖面拾取目标断层；空串 = 拾取时自动新建 F<N>。
    QString activeFaultId() const { return m_activeFaultId; }
    void setActiveFaultId(const QString &faultId) { m_activeFaultId = faultId; }
    // 层位图切割目标层位：SelectionContext::activeHorizon() 代理，空回 "H1"。
    QString activeHorizon() const;

    // ---- 编辑原语（全部入 undo 栈，push 即执行 redo）----
    // 新建命名断层（面板）；name 撞名自动去重。返回 faultId。
    QString addFault(const QString &name, const QString &interpreter = QString());
    // 剖面断层棒拾取：目标 = activeFaultId（空则自动新建断层，同命令内创建，
    // undo 时若该断层已被掏空则一并撤销）。返回 (faultId, stickId)，拒绝 → ("","").
    QPair<QString, QString> addStick(const FaultStick &stick);
    bool removeStick(const QString &faultId, const QString &stickId);
    bool removeFault(const QString &faultId);
    bool renameFault(const QString &faultId, const QString &newName);
    bool setFaultVisible(const QString &faultId, bool visible);
    // 层位切割多边形（层位图绘制）；同 (fault, horizon) 替换。faultId 空 →
    // activeFaultId（仍空则自动新建）。
    bool setCut(const QString &faultId, const FaultHorizonCut &cut);
    bool removeCut(const QString &faultId, const QString &horizon);
    bool setCutHangingSide(const QString &faultId, const QString &horizon,
                           FaultHangingSide side);

    // ---- 联动 ----
    // 广播断层选择（ids 为裸 faultId；载荷自动加前缀）。origin = 来源视图名。
    void selectFaults(const QStringList &faultIds, const QString &origin);
    QStringList selectedFaultIds() const { return m_selectedFaultIds; }
    static QString selectionId(const QString &faultId);
    static QString faultIdFromSelectionId(const QString &selectionId);

    // ---- catalog 角色 ----
    // seismic_survey 实体 + 其 fault 角色锚资产。FaultSet 非空且尚未挂链时
    // ensure 一次（幂等）。
    void setCatalogContext(DataCatalog *catalog, const QString &entityId,
                           const QString &assetId);

    // ---- 层位图切割镜像层 ----
    // 幂等创建内存 Polygon 图层并加入 QgsProject（组：断层解释）；
    // crsAuthId 如 "EPSG:32650"。模型变更时自动整刷要素。
    QgsVectorLayer *ensureMapLayer(const QString &crsAuthId);
    QgsVectorLayer *mapLayer() const;

signals:
    // 模型任何变更（命令 redo/undo 后）；视图据此整刷。
    void faultSetChanged();
    // 联动选择变化（fault: 前缀已剥；含本控制器广播后的回声）。
    void faultSelectionChanged(const QStringList &faultIds);

private:
    // 命令内部无 undo 原语：改模型 → 落盘 → 发信号。返回落盘是否成功。
    bool commitModel();
    void refreshMapLayer();
    void ensureFaultRoleLink();
    void onContextSelection(const QStringList &ids, const QString &origin);
    void onMapLayerSelectionChanged();

    FaultSetStore *m_store = nullptr;
    SelectionContext *m_selection = nullptr;
    FaultEditStack *m_editStack = nullptr;
    DataCatalog *m_catalog = nullptr;
    QString m_catalogEntityId;
    QString m_catalogAssetId;
    bool m_faultRoleLinked = false;
    QString m_activeFaultId;
    QStringList m_selectedFaultIds;
    QString m_lastError;
    FaultSet m_set;
    QPointer<QgsVectorLayer> m_mapLayer; // 权属 QgsProject；QPointer 防工程清理后悬空
};

} // namespace paleo::fault
