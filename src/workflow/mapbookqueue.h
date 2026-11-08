// 层：功能
#pragma once
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>

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
//      的确定性档位（预演前 N 版；未出的版同样记 cancelled，人读原因区分
//      「用户取消」与「预演限额」——P5-W4 不引入第三种页状态，汇总换算
//      = cancelled，旧字段口径不变）。
//   3. 落盘目录结构钉死，产物全部可查：
//        <outputDir>/<book>/pages/<格名>.<ext>      逐版成品
//        <outputDir>/<book>/index/<book>_index.<ext> 目录索引页（可选，分页后
//                                                    可能有 _2.. 兄弟文件）
//        <outputDir>/<book>/manifest.json            全册账（含失败明细）
//
// P5-W4：逐页状态显式入账——Result::pages 与 request.tiles 同序同量，页状态
// ok/failed/cancelled 与 succeeded/failed/cancelled 计数守恒；manifest、Result、
// UI 摘要同源生成。页成功 ≠ 索引/清单成功：装配段失败进 failures（index<0 的
// 整体项）且 complete() 连它一起算。
//
// P5-W3：提交时刻的输入冻结说明（FrozenInputs，纯值）——图层稳定身份/顺序/
// CRS/可解析的 asset/version/SHA、工程 CRS、版面参数与运行库版本，随 manifest
// 的 inputs 段入册；工程随后换版本不影响已导出册的对应关系（导出本就跑在快照
// 重建的私有工程上）。
//
// 线程池纪律（禁区：不自建线程池）：异步一律经 PaleoTaskService；本类只是
// 编排与记账，不碰 QGIS 画图（建版/渲染/销毁在 qgis/mapbookrender facade，
// P5-W6 收敛），不碰 QtWidgets。
//
// start() freezes project declarations on the owner thread, rebuilds a private
// project and exports all pages in the task pool. Catalogue files and SHA-256
// are prepared in a private staging tree; owner commits one guarded journal.
// run() is the synchronous core for service-less hosts and worker-private
// projects.
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
    int index = -1;     //!< 格序号；prepare/index/manifest 阶段的整体失败为 -1
    QString tile;       //!< 格名；整体失败时为 "*"
    QString stage;      //!< prepare/template/layout/export/register/index/manifest
    QString error;      //!< 人读原因
};

// P5-W3：图层输入冻结记录（owner 线程捕获的纯值；无 Qgs 类型）。
struct LayerInputRecord
{
    QString qgisId;     //!< QGIS 图层 id（快照重建时的稳定身份）
    QString paleoId;    //!< paleoLayerId（受管声明身份；未声明为空）
    QString name;       //!< 显示名（快照时刻）
    QString type;       //!< vector/raster/...
    QString crs;        //!< authid（无效 CRS 为空——如实，不填默认值）
    QString assetId;    //!< 可解析的受管资产 id（paleoAssetId；无则空）
    QString versionId;  //!< 可解析的版本 id（paleoVersionId；无则空）
    QString sha256;     //!< catalog 解析出的版本内容摘要（解析不到为空）
    bool managed = false; //!< versionId 可解析 = 受管来源；false = 显式未受管
    QString source;     //!< 数据源 URI（未受管来源的追溯面）
};

// P5-W3：整册冻结输入说明（随 Request 冻结，manifest inputs 段的数据源）。
struct FrozenInputs
{
    QVector<LayerInputRecord> layers; //!< 图层快照（含顺序——同 Request.layers 序）
    QString projectCrs;               //!< 快照工程 CRS authid（可空 = 未知）
    QString qgisRuntime;              //!< QGIS 运行库版本串
};

// P5-W4：逐页状态（与 request.tiles 同序同量）。
enum class PageStatus
{
    Ok,
    Failed,
    Cancelled
};

QString pageStatusToString( PageStatus status );

struct PageRecord
{
    int index = -1;         //!< 格序号（同 Tile.index）
    QString tile;           //!< 格名
    PageStatus status = PageStatus::Failed;
    QString file;           //!< 落盘成品绝对路径（仅 Ok 非空）
    qint64 bytes = 0;       //!< 成品字节数（Ok）
    QString sha256;         //!< 成品内容摘要（Ok 且可读；否则空）
    QString reason;         //!< Failed/Cancelled 的人读原因（可空）
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
    FrozenInputs inputs;                  //!< P5-W3：提交时刻冻结的输入说明
                                         //!<（start() 在 owner 线程填充；同步 run()
                                         //!<  由 prepareRun 按同规则补齐）
};

struct Result
{
    int total = 0;
    int succeeded = 0;
    int failed = 0;
    int cancelled = 0;
    QVector<Failure> failures; //!< 单版失败 + 装配段整体失败明细（可查）
    QVector<PageRecord> pages; //!< P5-W4：逐页状态（与 tiles 同序同量，守恒）
    QStringList files;         //!< 落盘成功的逐版文件
    QStringList registered;    //!< catalog 受管副本路径（与 files 一一对应）
    QStringList registeredIds; //!< catalog 资产 id（与 files 一一对应，回查元信息用）
    QString indexPage;         //!< 目录索引页主文件（未出则空）
    QStringList indexFiles;    //!< P5-W5：索引页全部文件（分页后含 _2.. 兄弟）
    QString manifest;          //!< manifest.json（写不出则空）
    QStringList warnings;      //!< P5-W3：缺源/降级 warning（快照层缺源等，入册可查）

    // failed 只计逐版失败（与 succeeded+cancelled==total 守恒）；装配阶段
    //（索引页/manifest.json）失败只进 failures——complete() 必须连它一起算，
    // 否则「整册完整」会对缺索引页/缺账本的册报喜。
    bool complete() const
    {
        return failed == 0 && cancelled == 0 && succeeded > 0 && failures.isEmpty();
    }
    // 人读一行：进度 + 失败/取消计数（任务页 detail 与测试断言共用）。
    QString summary() const;
    // P5-W7：交付摘要（含装配状态——索引页/清单是否齐、部分可用显式呈现）。
    QString deliverySummary() const;
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
    ~Exporter() override;

    // 异步：任务中心后台出图，所属线程只做快照与成果登记。
    // 返回 nullptr = 未绑定任务服务，或已有一册在导出（一次只跑一册）。
    PaleoTask *start( const Request &request );

    // 同步核心：异步分步与测试共用同一组阶段函数；task 可为 nullptr（无取消语义）。
    // 只能在本对象所属线程调用。
    Result run( PaleoTask *task, const Request &request );

    bool busy() const { return static_cast<bool>( m_async ); }
    Result lastResult() const { return m_last; }

  signals:
    void progress( int done, int total, const QString &tile );
    void finished( const Result &result );

  public:
    struct RunState; // 定义在 .cpp（含 QGIS 层类型）
    struct AsyncRun;

  private:
    bool prepareRun( RunState &state, PaleoTask *task );
    // 返回 false = 该在此处停（取消/预演上限），剩余版已记 cancelled。
    bool exportTileAt( RunState &state, int i, PaleoTask *task );
    void finishRun( RunState &state, PaleoTask *task );

    PaleoTaskService *m_tasks = nullptr;
    QgsProject *m_project = nullptr;
    Result m_last;
    std::shared_ptr<AsyncRun> m_async; // 仅所属线程读写
};

} // namespace PaleoMapBookQueue

Q_DECLARE_METATYPE( PaleoMapBookQueue::Result )
