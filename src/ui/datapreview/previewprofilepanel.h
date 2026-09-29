// 层：视图
#pragma once

#include <QWidget>

#include <qgspointxy.h>

#include <functional>

// ui/datapreview/previewprofilepanel — 层位剖面分析面板（P2 D5.1–D5.4）。
// 剖面线在画布上拖出（PreviewProfileTool → 分支采样 → addProfile），这里
// 画剖面小图：距离/值双轴、悬停读数（距离/值/坡度，D5.2）、多条剖面线
// 同图叠绘对比（D5.4）、导出 PNG/CSV（D5.3，目标路径由用户选择——非工程
// 写路径）。采样数据由 PreviewRasterAnalysis::sampleProfile 出。
class PreviewProfilePanel : public QWidget
{
    Q_OBJECT
  public:
    struct Sample
    {
      double distance = 0.0;
      double value = 0.0;
      bool valid = false;
    };
    struct Series
    {
      QString name;
      QColor color;
      QVector<Sample> samples;
      // 剖面端点（地图坐标，导出 CSV 头部记录）。
      QgsPointXY p1;
      QgsPointXY p2;
    };

    explicit PreviewProfilePanel( QWidget *parent = nullptr );

    void addProfile( const QString &name, const QVector<Sample> &samples,
                     const QgsPointXY &p1, const QgsPointXY &p2 );
    void setProfiles( const QVector<Series> &series );
    void clearProfiles();
    int profileCount() const { return m_series.size(); }
    const QVector<Series> &series() const { return m_series; }

    // 供测试注入文件对话框目标（不弹窗）。
    void setExportTargetForTesting( const QString &csvPath, const QString &pngPath );

  signals:
    // 悬停读数文本变化（测试断言面）：空串 = 无悬停。
    void hoverReadoutChanged( const QString &text );

  private:
    friend class ProfileChart;
    void rebuildLegend();
    void exportCsv();
    void exportPng();
    bool writeCsv( const QString &path ) const;
    bool writePng( const QString &path ) const;

    class ProfileChart; // 内部绘制件
    ProfileChart *m_chart = nullptr;
    class QLabel *m_legend = nullptr;
    QVector<Series> m_series;
    QString m_csvTarget; // 测试注入
    QString m_pngTarget;
};
