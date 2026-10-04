// 层：功能
#pragma once
#include <QString>
#include <QVector>

#include "../metadata/layouttemplatestore.h"

class DataCatalog;
class QgsLayout;

// workflow/layouttemplatelibrary — 版式模板库编排（方向 25 M1b）。
//
// 图件资产纪律：模板内容不落游离文件——序列化字节经 catalog 受管区落位
//（type=layout_template，stage=INTERMEDIATE，SHA-256 版本），库语义（名字/
// 重命名/删除）走 project.sqlite 的 layout_templates 表。本类只做编排与
// 字节搬运，不碰 QtWidgets。
//
// 版本策略：
//   saveAs   → 新资产 + v1 + 新库行（同名已存在 = 失败，先改名或走 save）
//   save     → 同资产新版本（versionNumber+1）+ 行的 version_id/sha256 前移；
//              每次保存都记新版本——QGIS 模板序列化 round-trip 非字节稳定
//             （比例尺框重算等），「内容未变」探测不可靠，账本如实记录
//   apply    → 受管文件读回 → loadFromTemplate(clearExisting)
//   rename/remove → 只动库表（catalog 账本不回擦，历史仍可追溯）
namespace PaleoLayoutTemplateLibrary
{

struct SaveOutcome
{
  bool ok = false;
  QString error;
  QString templateId;
  QString assetId;
  QString versionId;
  QString sha256;
  QString managedPath;
};

class Library
{
  public:
    Library( DataCatalog *catalog, LayoutTemplateStore *store, const QString &projectDir );

    //! 另存为新模板（库内名字唯一）。
    SaveOutcome saveAs( QgsLayout *layout, const QString &name, const QString &kind,
                        const QString &pageSize, bool landscape );

    //! 重存既有模板（同资产新版本）。
    SaveOutcome save( QgsLayout *layout, const QString &templateId );

    //! 套用模板到版面（清现有内容项，页面/绑定按模板恢复）。
    bool apply( QgsLayout *layout, const LayoutTemplateInfo &tpl, QString *error = nullptr );

    //! 模板当前内容的受管文件路径（空 = 解析失败）。
    QString contentPath( const LayoutTemplateInfo &tpl ) const;

    QVector<LayoutTemplateInfo> list() const;
    LayoutTemplateInfo byName( const QString &name ) const;
    bool rename( const QString &templateId, const QString &newName, QString *error = nullptr );
    bool remove( const QString &templateId, QString *error = nullptr );

  private:
    SaveOutcome registerContent( QgsLayout *layout, const QString &assetId, const QString &versionHint );

    DataCatalog *m_catalog = nullptr;
    LayoutTemplateStore *m_store = nullptr;
    QString m_projectDir;
};

} // namespace PaleoLayoutTemplateLibrary
