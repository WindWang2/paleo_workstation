// 层：数据
#pragma once
#include <QHash>
#include <QImage>
#include <QSize>
#include <QString>
#include <QVector>

class QBrush;

// services/imagelod — 图片道装载 LOD（方向 79）：岩心/薄片照片的统一装载
// 面，连井剖面（任务线程缩略装载）、综合柱状图（GUI 装配缩略）、井附件
// 面板与锚深对话框（预览）共用。此前各消费方各自 QImage 全分辨率常驻，
// 几百张高分辨率照片即性能墙。
//
// LodPolicy（定案口径，改动前先对照 ledger 与 docs/progress 注记）：
//   两级装载——缩略级：最长边 256px，装载即生成、常驻内存（道内容常态
//   只需要道宽 ~110-220px 的显示）；原图级：驻磁盘，按需全载。
//   全载触发：道内「可见宽度」（场景宽 × 视图缩放）> 缩略宽 × 2.0 时，
//   绘制路径同步全载一次并进 LRU（8 张，按最后使用淘汰）。放大检视是
//   单张行为，同步装载可接受；批量路径（剖面装载）永不触发原图级。
//   解码期降采样：QImageReader::setScaledSize——装载内存峰值 = 缩略字节
//   而非全图字节（JPEG 走 libjpeg 1/2^n 快速路径，其余格式解码后缩放，
//   峰值仍受 QImageReader 内部单图约束）。
//   EXIF Orientation：QImageReader::setAutoTransform(true) 统一应用——
//   装载路径唯一，旋转样张在道内正立是本策略的行为面。
namespace paleo::imagelod
{

struct LodPolicy
{
  static constexpr int kThumbnailEdge = 256;      // 缩略级最长边 px
  static constexpr double kFullLoadFactor = 2.0;  // 可见宽超缩略宽该系数 → 全载
  static constexpr int kFullCacheEntries = 8;     // 原图级 LRU 张数
};

// 装载结果：缩略位图 + 原图几何。原图字节不随此结构驻留。
struct TrackImage
{
  QImage thumbnail;   // 最长边 ≤ edge；EXIF 已应用
  QSize fullSize;     // 原图像素尺寸（含 EXIF 方向修正）
  bool hasAlpha = false;
  bool isNull() const { return thumbnail.isNull(); }
};

// 缩略装载（线程安全——任务线程/GUI 线程皆可）。失败回 isNull() 位图，
// 调用方如实告警（不猜不吞）。edge ≤ 0 按 LodPolicy 缺省。
TrackImage loadThumbnail(const QString &path,
                         int edge = LodPolicy::kThumbnailEdge);

// 原图全载（EXIF 同口径）。放大检视/预览对话框用；不做驻留。
QImage loadFull(const QString &path);

// 原图级 LRU：道内放大按需全载的驻留面。GUI 线程持有（绘制路径专用；
// 无锁——不跨线程）。key 惯例 "path|imageVersion"（数据变更即换键）。
class FullImageCache
{
  public:
    // 命中即回，不装载、不刷新使用序（绘制命中路径 O(1)）。
    QImage peek(const QString &key) const;
    // 命中刷新 LRU；未命中装载（失败回空图不占额）。
    QImage acquire(const QString &key, const QString &path);
    void clear();
    int size() const { return m_map.size(); }
    static FullImageCache &shared(); // 视图侧共用单例（井面板/剖面/柱状图）

  private:
    QVector<QString> m_order;            // 尾 = 最近使用
    QHash<QString, QImage> m_map; // path|version → 原图（EXIF 已应用）
};

// 透明图棋盘底笔刷：纸面图件口径——中性灰固定双色（浅 #EEEEEE / 深
// #BBBBBB），不随 UI 暗色翻转（剖面/柱状图是文档不是 chrome）。cellPx
// 为单格像素边长。QImage 纹理（无 QPixmap——services 不依赖 GUI 实例）。
QBrush alphaCheckerboard(int cellPx = 6);

} // namespace paleo::imagelod
