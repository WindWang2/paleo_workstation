// 层：视图
#pragma once
#include <QWidget>
#include <QVector>

#include "../../workflow/mapbook.h"
#include "../../workflow/mapbookqueue.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTextEdit;

// ui/layout — PaleoMapBookPanel：地图册批量参数配置面板（视图层）。
//
// 层纪律：视图只发信号，不编排也不渲染——点「开始批量导出」只发
// batchRequested()，参数由本面板的 getter 与 previewTiles() 现算（格序列是
// 纯功能层 workflow/mapbook 的活），真正的跑批在功能层
// workflow/mapbookqueue，画图在 qgis/mapbooklayout。
//
// 面板自带三件事：参数录入、进度回显（setBusy/setProgress）、失败明细回显
// （appendLog）——都是被动显示，不做任何业务判断。
class PaleoMapBookPanel : public QWidget
{
    Q_OBJECT

  public:
    explicit PaleoMapBookPanel( QWidget *parent = nullptr );

    // ---- 参数（读侧：组装方据此拼 Request；写侧：外部把当前工区范围灌进来）----
    PaleoMapBook::Area area() const;
    void setArea( const PaleoMapBook::Area &area );
    int cols() const;
    int rows() const;
    void setGrid( int cols, int rows );
    PaleoMapBook::Order order() const;
    QString bookName() const;
    void setBookName( const QString &name );
    QString outputDir() const;
    void setOutputDir( const QString &dir );
    QString titlePattern() const;
    QString footerPattern() const;
    double dpi() const;
    PaleoMapBookQueue::Format format() const;
    bool withIndexPage() const;
    int previewLimit() const; //!< 0 = 不设上限（全部）

    // 当前参数下的格序列（纯计算，不发信号、不落盘）。
    QVector<PaleoMapBook::Tile> previewTiles( QString *error = nullptr ) const;

    // ---- 回显（被动）----
    void setBusy( bool busy );
    void setProgress( int done, int total );
    void appendLog( const QString &line );
    void clearLog();

  signals:
    // 视图只发信号：批量参数已配好，请功能层编排（参数走 getter/previewTiles）。
    void batchRequested();
    void cancelRequested();

  private:
    void buildUi();
    void onStart();
    void onCancel();
    void onPickDir();

    QLineEdit *m_book = nullptr;
    QLineEdit *m_dir = nullptr;
    QDoubleSpinBox *m_xMin = nullptr;
    QDoubleSpinBox *m_yMin = nullptr;
    QDoubleSpinBox *m_xMax = nullptr;
    QDoubleSpinBox *m_yMax = nullptr;
    QSpinBox *m_cols = nullptr;
    QSpinBox *m_rows = nullptr;
    QComboBox *m_order = nullptr;
    QComboBox *m_format = nullptr;
    QComboBox *m_dpi = nullptr;
    QLineEdit *m_title = nullptr;
    QLineEdit *m_footer = nullptr;
    QCheckBox *m_index = nullptr;
    QSpinBox *m_preview = nullptr;
    QPushButton *m_start = nullptr;
    QPushButton *m_cancel = nullptr;
    QLabel *m_status = nullptr;
    QProgressBar *m_progress = nullptr;
    QTextEdit *m_log = nullptr;
};
