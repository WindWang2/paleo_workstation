// 层：QGIS 封装
#pragma once

#include <QHash>
#include <QImage>
#include <QString>
#include <QStringList>

#include <memory>

class QTemporaryDir;

// qgis/previewrendercache — 预览渲染结果缓存（P2 D6.2/D6.6）。
// 键 = assetId + versionId + 范围（取整）+ 视口尺寸；两层：
//   · 内存 LRU（默认 16 张，超限逐最远——D6.6 内存预算）；
//   · 磁盘 PNG（QTemporaryDir 级目录，进程生命期）。
// 命中给调用方一张立即可上屏的 overlay 图（D6.2 语义），真渲由画布
// renderCompleted 让位。
class PreviewRenderCache
{
  public:
    static PreviewRenderCache &instance();

    // 组键：范围/尺寸取整（1e-3 m 与 8px 粒度）——缩放微抖不炸缓存。
    static QString makeKey( const QString &assetId, const QString &versionId,
                            const class QgsRectangle &extent, int widthPx, int heightPx );

    // 命中返回非空图并 LRU touch；未命中返回空 QImage。
    QImage lookup( const QString &key );
    void store( const QString &key, const QImage &image );

    int memoryEntryCount() const { return m_mem.size(); }
    int memoryLimit() const { return kMemLimit; }
    QString diskDirPath() const;

    void clearMemoryForTesting();

  private:
    PreviewRenderCache();
    ~PreviewRenderCache(); // #419：m_dir 出 unique_ptr——进程退出真正清盘
    static constexpr int kMemLimit = 16;
    static constexpr int kDiskLimit = 64; // #376 磁盘层上限（超出逐最远删盘）

    std::unique_ptr<QTemporaryDir> m_dir;
    QHash<QString, QImage> m_mem;
    QStringList m_lru;     // 最近使用在尾
    QStringList m_diskLru; // 磁盘条目 LRU（与 m_dir 内 PNG 一一对应）
};
