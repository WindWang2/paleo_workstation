// 层：数据
#pragma once
#include <QString>
#include <QVector>

// metadata/layouttemplatestore — 版式模板库的工程级语义索引（方向 25 M1）。
//
// catalog 是 append-only 的内容账本（版本/SHA/受管字节），没有 rename/delete
// 语义；「可另存/重命名/删除」的库语义由本表承载（MapVersionStore 先例：
// project.sqlite 的可变索引 + catalog 的不可变内容）。一行 = 一个用户模板：
//   内容 → catalog 资产 layout_template 的当前版本（asset_id/version_id/sha256）
//   语义 → 名字（唯一）、图件种类 key、页面规格、时间戳
// 删除模板只删本表行——catalog 里的历史版本仍可追溯（账本不回擦）。
struct LayoutTemplateInfo
{
  QString id;         //!< "lt-N"，库内顺序号
  QString name;       //!< 用户名（库内唯一）
  QString kind;       //!< 图件种类 key（standardelements 词表）或 "custom"
  QString pageSize;   //!< "A4" / "A3" / ...
  bool landscape = true;
  QString assetId;    //!< catalog 资产 id（内容）
  QString versionId;  //!< 当前内容版本 id
  QString sha256;     //!< 当前内容版本摘要
  QString createdUtc; //!< ISO-8601 UTC
  QString updatedUtc; //!< ISO-8601 UTC

  bool isNull() const { return id.isEmpty(); }
};

class LayoutTemplateStore
{
  public:
    explicit LayoutTemplateStore(const QString &metaSqlitePath);

    bool open(QString *error = nullptr); // creates schema if absent

    // 新模板（另存为）：名字库内唯一。返回新 id，失败回空串 + error。
    QString create(const QString &name, const QString &kind, const QString &pageSize,
                   bool landscape, const QString &assetId, const QString &versionId,
                   const QString &sha256, QString *error = nullptr);

    // 重存：同一模板换内容版本（catalog 新版本落位后调用）。
    bool updateContent(const QString &id, const QString &versionId, const QString &sha256,
                       QString *error = nullptr);

    // 重命名（只动本表语义，不动 catalog 字节）。
    bool rename(const QString &id, const QString &newName, QString *error = nullptr);

    // 删除（只删本表行；catalog 历史版本保留）。
    bool remove(const QString &id, QString *error = nullptr);

    QVector<LayoutTemplateInfo> templates() const;         // 按名字升序
    LayoutTemplateInfo byName(const QString &name) const;  //!< 空回 isNull 模板
    LayoutTemplateInfo byId(const QString &id) const;      //!< 空回 isNull 模板
    bool nameExists(const QString &name) const;

  private:
    QString m_dbPath;
};
