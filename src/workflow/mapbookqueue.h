// 层：功能
#pragma once
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include "mapbook.h"

class DataCatalog;
class PaleoTask;
class PaleoTaskService;
class QgsMapLayer;
class QgsProject;

// workflow/mapbookqueue — 地图册批量导出队列（功能层编排）。
//
// 契约三条（Oracle 3）：
//   1. 逐版落盘，单版失败不拖死整队——失败留在 Result::failures（含格号/阶段/
//      原因），其余版照出。
//   2. 取消是协作式的：worker 每版前自查 task->cancelRequested()，已出的版
//      保留，未出的记为 cancelled（不谎报成功）。previewLimit ≥0 是同一语义
//      的确定性档位（预演前 N 版）。
//   3. 落盘目录结构钉死，产物全部可查：
//        <outputDir>/<book>/pages/<格名>.<ext>      逐版成品
//        <outputDir>/<book>/index/<book>_index.<ext> 目录索引页（可选）
//        <outputDir>/<book>/manifest.json            全册账（含失败明细）
//
// 线程池纪律（禁区：不自建线程池）：异步一律经 PaleoTaskService；本类只是
// 编排与记账，不碰 QGIS 画图（画图在 qgis/mapbooklayout）、不碰 QtWidgets。
namespace PaleoMapBookQueue
{

// 导出格式（功能层口径；落到 QGIS 层时映射为 PaleoLayoutExport::Format）。
enum class Format
{
    Png,
    Pdf
};

struct Failure
{
    int index = -1;     //!< 格序号；prepare/index 阶段的整体失败为 -1
    QString tile;       //!< 格名；整体失败时为 "*"
    QString stage;      //!< prepare/template/layout/export/register/index
    QString error;      //!< 人读原因
};

struct Request
{
    QString book;                         //!< 图册名（落盘目录名，须是合法路径段）
    QString outputDir;                    //!< 输出根目录
    QVector<PaleoMapBook::Tile> tiles;    //!< 格序列（workflow/mapbook 产出）
    QString titlePattern;                 //!< 版面标题模板（%{...} 变量）
    QString footerPattern;                //!< 页脚模板（格号/中心坐标/CRS）
    QList<QgsMapLayer *> layers;          //!< 每版地图项图层（同源，逐版只换范围）
    double dpi = 300.0;
    Format format = Format::Png;
    bool withIndexPage = false;           //!< 报告装配：出目录索引页
    bool landscape = true;                //!< 版面 A4 横版/竖版
    DataCatalog *catalog = nullptr;       //!< 非空则逐版登记产物
    QString projectDir;                   //!< catalog 受管区根（登记需要）
    QString horizon;                      //!< 层位（进变量表，可空）
    QString crs;                          //!< CRS 说明（可空 → 缺省文案）
    QString date;                         //!< ISO 日期（可空 → 当天）
    int previewLimit = -1;                //!< ≥0：只出前 N 版（预演/确定性取消）
};

struct Result
{
    int total = 0;
    int succeeded = 0;
    int failed = 0;
    int cancelled = 0;
    QVector<Failure> failures; //!< 单版失败明细（可查）
    QStringList files;         //!< 落盘成功的逐版文件
    QStringList registered;    //!< catalog 受管副本路径（与 files 一一对应）
    QStringList registeredIds; //!< catalog 资产 id（与 files 一一对应，回查元信息用）
    QString indexPage;         //!< 目录索引页（未出则空）
    QString manifest;          //!< manifest.json（写不出则空）

    bool complete() const { return failed == 0 && cancelled == 0 && succeeded > 0; }
    // 人读一行：进度 + 失败/取消计数（任务页 detail 与测试断言共用）。
    QString summary() const;
};

// 落盘目录结构（钉死，单点事实）。
QString bookRoot( const Request &request );
QString pagesDir( const Request &request );
QString pagePath( const Request &request, const PaleoMapBook::Tile &tile );
QString indexPath( const Request &request );
QString manifestPath( const Request &request );
QString extensionFor( Format format );

class Exporter : public QObject
{
    Q_OBJECT

  public:
    explicit Exporter( PaleoTaskService *tasks, QgsProject *project, QObject *parent = nullptr );

    // 异步：经任务服务排队（返回 nullptr = 未绑定任务服务）。
    PaleoTask *start( const Request &request );

    // 同步核心：worker 与测试共用这一条路径；task 可为 nullptr（无取消语义）。
    Result run( PaleoTask *task, const Request &request );

    Result lastResult() const { return m_last; }

  signals:
    void progress( int done, int total, const QString &tile );
    void finished( const Result &result );

  private:
    PaleoTaskService *m_tasks = nullptr;
    QgsProject *m_project = nullptr;
    Result m_last;
};

} // namespace PaleoMapBookQueue

Q_DECLARE_METATYPE( PaleoMapBookQueue::Result )
