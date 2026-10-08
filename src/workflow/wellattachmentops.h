// 层：功能
#pragma once
#include <QString>
#include <QVector>

class DataCatalog;

// workflow/wellattachmentops — 井附件（core/lab_analysis 图片）锚深编辑
// 编排（方向 79）：DataCatalog::updateVersionExtra 通道的校验面 + 管理面板
// 列表面。功能层不建对话框——输入解析产出结构化 reason，用户文案由视图
// 层 tr() 出（reasonText() 是唯一文案出口，供面板/对话框共用）。
namespace paleo
{

// 井附件面板一行：core/lab_analysis 链接 → 当前版本。无锚照片如实列
//（hasAnchor=false——「未锚定」正是本方向要暴露给后补编辑的面）。
struct WellAttachmentRow
{
  QString assetId;
  QString versionId;
  QString fileName;      // 版本文件名（缺则资产显示名）
  QString displayName;   // 资产显示名（原始 basename）
  QString role;          // core | lab_analysis
  double depthMd = 0.0;
  bool hasAnchor = false;
  QString anchorSource;  // filename | manual | 空（无锚）
  int versionNumber = 1;
  QString stage;         // RAW | DERIVED | …
  bool managed = true;
  QString path;          // 解析后绝对路径（缩略图/预览用；空 = 不可解析）
  bool unresolved = false;
};

// 输入解析的拒收原因（结构化，不夹用户文案）。
enum class DepthInputStatus
{
  Ok,          // 解析成功（out 有效）
  Clear,       // 空白输入 = 清锚（回到未锚定态）
  NotFinite,   // 非数字（abc/1.2.3）
  NonPositive, // ≤0（深度没有非正数）
  BadUnit      // 数字带非 m 后缀（如 100ft——单位由对话框固定 m，不换算不猜）
};

class WellAttachmentOps
{
  public:
    explicit WellAttachmentOps(DataCatalog *catalog);

    // 井的全部 core/lab_analysis 附件行（含未锚定/未决——管理面要如实
    // 全列，与 facade imagesFor「有锚才收」的道内容口径互补）。按
    // （有锚按 depthMd 升序在前，未锚定按文件名）排序。
    QVector<WellAttachmentRow> rowsForWell(const QString &wellId,
                                           const QString &projectDir) const;

    // 解析用户输入（手工口径定案见 ledger）：接受有限正数，容忍首尾空白
    // 与 m/米 后缀；空白 = 清锚；其余拒收并给 reason。
    static DepthInputStatus parseDepthInput(const QString &text, double *out);

    // 编辑落库：经 updateVersionExtra 就地更新（含审计 history/source）。
    // status 是解析结果；error 是 catalog 侧错误（落库失败原因）。
    bool setDepthAnchor(const QString &versionId, const QString &text,
                        DepthInputStatus *status = nullptr,
                        QString *error = nullptr);

    // reason 的用户文案（唯一出口——视图层经此 tr()）。
    static QString reasonText(DepthInputStatus status);

  private:
    DataCatalog *m_catalog = nullptr;
};

} // namespace paleo
