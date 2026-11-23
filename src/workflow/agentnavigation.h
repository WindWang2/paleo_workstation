// 层：功能
#pragma once
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <functional>

class DataCatalog;
class QTimer;

// 三方宿主访问接口（SelectionAccess / MapAccess / PreviewAccess）见
// services/agentnavigationaccess.h（数据层纯 Qt 契约——qgis 封装层实现
// MapAccess 时不得反向 include 功能层，接口落点据此上移；命名空间同为
// paleo::agent::navigation，本头内直接使用）。
#include "../services/agentnavigationaccess.h"

// workflow/agentnavigation.h — I4：语义导航原子的功能层共享件。
//
// 三个宿主访问接口只带纯 Qt 类型：生产实现分别落 ui/agentnavigationadapter
// （选择 + 原生预览页）与 qgis/agentmapnavigation（主画布定位），由组装根
// 注入——功能层不出现 QtWidgets/Qgs 符号（层契约 + qgs-spread 闸）。
//
// AgentNavigation 编排：受信请求绑定（catalog/documentId 来自 runner 宿主
// 上下文，不信实参）、严格外部档校验共用入口、目标业务身份解析（W2）、
// 单在途请求 + 有限截止 + 迟到回执丢弃（W8）。选择/定位/预览是临时视图态：
// 效果恒 Read，不写 Catalog 业务状态、不改 Presentation 写门。

namespace paleo::agent::navigation {

// 结果契约：ok=false 时 code 为机器错误码；payload 由编排层补 contract。
QJsonObject navigationResult(bool ok, const QString &code,
                             const QJsonObject &facts = {});

// 严格外部档共用入口（AiToolSpec 通用档之外的互斥分支/集合形状/长度界）。
// runner 入口与 readiness 共用，不复制第二份口径。空串 = 通过；非空 =
// 机器错误码；field 给首个违规字段（可空）。
QString validateNavigationArguments(const QString &name, const QJsonObject &args,
                                    QString *field = nullptr);

class AgentNavigation : public QObject {
  Q_OBJECT
public:
  explicit AgentNavigation(QObject *parent = nullptr);
  ~AgentNavigation() override;

  // 宿主安装（owner 守卫寿命；成对供给，无 owner 按未安装处理）。
  void setSelectionAccess(SelectionAccess *access, QObject *owner);
  void setMapAccess(MapAccess *access, QObject *owner);
  void setPreviewAccess(PreviewAccess *access, QObject *owner);

  struct Environment {
    DataCatalog *catalog = nullptr; // 受信宿主目录（runner 上下文注入）
    QString documentId;             // 受信文档绑定（不收实参）
  };
  using Completion = std::function<void(bool ok, const QJsonObject &payload)>;

  // readiness 共用的存在性/可用性判定：六工具各自所需 provider 缺位的
  // 机器错误码；空串 = 可用。catalogRequired 区分需要目录的目标解析工具。
  QString unavailabilityReason(const QString &name, bool *catalogRequired = nullptr) const;

  // 执行一个导航原子。done 在 GUI 线程回调恰一次（同步完成也经队列派发，
  // #281 纪律）；同刻只允许一个在途请求——新 run 作废旧请求（旧 done 不再
  // 触发），cancel() 同理。迟到回执按请求代次丢弃，不产生第二终态。
  void run(const QString &name, const QJsonObject &args, const Environment &env,
           const Completion &done);
  // 宿主/会话失效：丢弃在途请求（不回调——调用方自行回报 stale）。
  void cancel();

private:
  struct Pending {
    quint64 serial = 0;
    Completion done;
  };
  void finish(quint64 serial, bool ok, const QJsonObject &payload);
  void startDeadline(int ms);
  bool beginRequest(const QString &name, const Completion &done, quint64 *serial);
  // 目标解析（W2）：全部 ID 必须是当前工程的井实体；失败返回非空错误码并
  // 填 unknown（有界）。
  QString resolveWellIds(const QStringList &ids, DataCatalog *catalog,
                         QStringList *resolved) const;
  QString resolveWellViewTarget(const QJsonObject &args, DataCatalog *catalog,
                                QString *assetId, QString *versionId, QString *wellId) const;
  QString resolveSeismicTarget(const QJsonObject &args, DataCatalog *catalog,
                               QString *assetId, QString *versionId, QString *axis,
                               int *lineNumber) const;

  SelectionAccess *m_selection = nullptr;
  QPointer<QObject> m_selectionOwner;
  MapAccess *m_map = nullptr;
  QPointer<QObject> m_mapOwner;
  PreviewAccess *m_preview = nullptr;
  QPointer<QObject> m_previewOwner;
  quint64 m_requestSerial = 0;      // 单调；每个在途请求持有自己的代次
  quint64 m_activeSerial = 0;       // 0 = 无在途
  Completion m_activeDone;
  QTimer *m_deadline = nullptr; // 懒建；单发截止（超时=终态，迟到回执丢弃）
  int m_deadlineMs = 0;             // 当前请求的截止（0 = 同步完成，无计时）
  int m_generation = 0;             // cancel()/析构 ++：作废迟到回调
};

} // namespace paleo::agent::navigation
