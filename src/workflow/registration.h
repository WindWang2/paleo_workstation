// 层：功能
#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

class DataImportService;
class QgisLayerService;

// workflow/registration — D11 临时配准编排（W3：从主窗下沉）。
//
// 手工仿射参数 → GeoAffineParams → DERIVED GeoJSON 版本登记（catalog 写）
// → 「临时配准 · 名」矢量图层实例化（告警橙虚线样式）→ 计数信号。
// 壳只接信号做视图动作：状态栏文案、画布水印开关、zoomToLayer。
// 本类不碰控件、不弹窗。
class RegistrationWorkflow : public QObject
{
  Q_OBJECT

  public:
    RegistrationWorkflow(DataImportService *svc, QgisLayerService *layerSvc,
                         QObject *parent = nullptr);

    // 手工仿射 → DERIVED 版本登记 → 图层实例化。失败发 registrationFailed；
    // 成功发 provisionalRegistered + provisionalLayerCountChanged。
    void applyProvisionalRegistration(const QString &assetId,
                                      const QVariantMap &params);

    int provisionalLayerCount() const { return m_provisionalLayers; }

  signals:
    // 失败原因如实（壳写状态栏 + QgsMessageLog）。
    void registrationFailed(const QString &message);
    // 登记+实例化成功（壳：状态栏文案 + zoomToLayer(layerId)）。
    void provisionalRegistered(const QString &layerId, const QString &title,
                               int featureCount);
    // 临时配准图层计数（单向锁存语义：只增）。壳按 n>0 开水印。
    void provisionalLayerCountChanged(int n);

  private:
    DataImportService *m_svc = nullptr;
    QgisLayerService *m_layerSvc = nullptr;
    int m_provisionalLayers = 0;
};
