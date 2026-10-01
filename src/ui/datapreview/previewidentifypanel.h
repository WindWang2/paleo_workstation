// 层：视图
#pragma once

#include <QWidget>

#include "../../qgis/previewidentify.h"

#include <functional>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTableWidget;
class QgsVectorLayer;

// ui/datapreview/previewidentifypanel — identify 结果面板 + 矢量全表模式
//（P2 D2.5/D7.x）。结果列表 + 属性卡（字段/值表，可复制行）+ 导出 CSV；
// 空命中给「未命中任何要素」反馈（D7.6）。
class PreviewIdentifyPanel : public QWidget
{
    Q_OBJECT
  public:
    explicit PreviewIdentifyPanel( QWidget *parent = nullptr );

    void setResults( const QVector<PreviewIdentifyResult> &results );
    int resultCount() const { return m_results.size(); }
    const QVector<PreviewIdentifyResult> &results() const { return m_results; }

    // 闪烁定位回调（D7.2）：宿主页接线到 QgsMapCanvas::flashFeatureIds。
    std::function<void( QgsMapLayer *, const QgsFeatureIds & )> flashHandler;

  signals:
    void resultsChanged();
    void exportCsvRequested( const QString &path ); // 写目标由分支/宿主落（测试注路径）
    void flashRequested( QgsMapLayer *layer, QgsFeatureId fid );

  private:
    void showResultDetails( int index );
    void exportCsv();
    void flashCurrent();

    QVector<PreviewIdentifyResult> m_results;
    QListWidget *m_resultList = nullptr;
    QTableWidget *m_attrTable = nullptr;
    QLabel *m_emptyLabel = nullptr;
    QString m_csvTarget; // 测试注入
};

// D7.3 全表模式：矢量资产属性表分页浏览（页大小 100）+ 点列头排序 +
// 列过滤（选列 + 包含文本）。非模态对话框由分支 new（父 = 预览页）。
class PreviewAttributeTableDialog : public QWidget
{
    Q_OBJECT
  public:
    PreviewAttributeTableDialog( QgsVectorLayer *layer, QWidget *parent = nullptr );

    int pageSize() const { return 100; }
    int pageCount() const;
    int currentPage() const { return m_page; }
    void goToPage( int page );
    // 过滤（列序号 -1 = 全列；文本包含）。空文本 = 不过滤。
    void setFilter( int column, const QString &text );

  protected:
    void closeEvent( QCloseEvent *event ) override;

  private:
    void rebuild();
    QStringList filteredFeatureIds() const; // 过滤后参与分页的要素 id

    QgsVectorLayer *m_layer = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_emptyLabel = nullptr; // goal/ui-experience-polish：过滤空结果指引
    QLineEdit *m_filterEdit = nullptr;
    QComboBox *m_filterColumn = nullptr;
    QLabel *m_pageLabel = nullptr;
    QPushButton *m_prevBtn = nullptr;
    QPushButton *m_nextBtn = nullptr;
    int m_page = 0;
    int m_filterColumnIdx = -1;
    QString m_filterText;
};
