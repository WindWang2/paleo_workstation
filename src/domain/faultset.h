// 层：数据
#pragma once
#include <QByteArray>
#include <QPair>
#include <QString>
#include <QVector>
#include <QVariantMap>

// domain/faultset — 断层解释数据模型（goal/fault-interpretation）。
// 纯值类型，无 Qgs* 依赖（§25：domain 不漏 QGIS）；全部经 QVariantMap /
// JSON 往返以落 project.sqlite（types.h 同款约定）。
//
// 结构：FaultSet（命名断层集，可整体序列化）
//   └ Fault（命名断层实体：F1/F2…，显隐，解释者）
//       ├ FaultStick（断层棒：剖面拾取折线，FaultSectionRef 定位 + TWT 范围）
//       └ FaultHorizonCut（层位切割：某层位平面图上的多边形 + 上盘/下盘关系）
//
// 边界（Oracle #6）：不做断层封闭性/断距计算；结构留 QVariantMap extra
// 扩展位即可。
namespace paleo::fault {

// 断层棒所属剖面的定位（IL/XL 切片或任意线）。
// 自带 Kind 枚举而不复用 SgySliceType：domain 不依赖 services。
struct FaultSectionRef {
    enum Kind { Inline = 0, Xline = 1, Arbitrary = 2 };

    int kind = Inline;
    int index = 0;       // Inline/XLine 线号
    QString pathId;      // 任意线身份：路径点串 "il,xl;il,xl;…"（同路径重提取可复现）
    QString displayName; // "IL 120" / "XL 34" / "任意线 (120,200;121,201)"

    bool isArbitrary() const { return kind == Arbitrary; }
    // 剖面身份键：Inline/XLine 用 kind+index，任意线用 pathId。
    QString matchKey() const;
    QVariantMap toMap() const;
    static FaultSectionRef fromMap(const QVariantMap &m);
};

// 断层棒：剖面上的折线拾取（traceFrac 0..1 剖面横向归一，twtMs 纵向）。
// traceFrac 归一使棒在剖面列数变化（重提取）后仍可回显。
struct FaultStick {
    QString id;
    FaultSectionRef section;
    QVector<QPair<double, double>> points; // (traceFrac, twtMs)
    QString interpreter;

    double twtMinMs() const;
    double twtMaxMs() const;
    QVariantMap toMap() const;
    static FaultStick fromMap(const QVariantMap &m);

    bool operator==(const FaultStick &o) const;
    bool operator!=(const FaultStick &o) const { return !(*this == o); }
};

// 上盘方向：沿切割多边形首边绘制方向的左/右侧（Unknown = 未定）。
// 只存解释关系，不推导几何（求交/裁剪为后续扩展）。
enum class FaultHangingSide { Unknown = 0, Left = 1, Right = 2 };

// 断层在某层位平面图上的切割多边形（WKT，工程 CRS）。
// 同一断层对每个层位独立一条（Oracle #4）。
struct FaultHorizonCut {
    QString horizon;      // 层位名（H1…）
    QString wkt;          // Polygon WKT
    FaultHangingSide hangingSide = FaultHangingSide::Unknown;
    QVariantMap extra;    // 前向扩展位（如后续断距/封堵属性）按原样往返

    QVariantMap toMap() const;
    static FaultHorizonCut fromMap(const QVariantMap &m);

    bool operator==(const FaultHorizonCut &o) const;
    bool operator!=(const FaultHorizonCut &o) const { return !(*this == o); }

    static QString hangingSideToString(FaultHangingSide side);
    static FaultHangingSide hangingSideFromString(const QString &s);
};

// 命名断层实体。
struct Fault {
    QString id;         // "f-1"（FaultSet 分配，稳定）
    QString name;       // "F1"（显示名，集内唯一）
    bool visible = true;
    QString interpreter;
    QVector<FaultStick> sticks;
    QVector<FaultHorizonCut> cuts; // 每层位至多一条（setCut 替换语义）
    QVariantMap extra;  // 前向扩展位

    const FaultStick *stickById(const QString &stickId) const;
    QVariantMap toMap() const;
    static Fault fromMap(const QVariantMap &m);
};

// 断层集：应用内唯一权威模型，JSON 整体序列化（落 project.sqlite 单行）。
class FaultSet {
public:
    // ---- 命名断层实体管理 ----
    // 新建断层；name 撞名时自动去重（"F" → "F2"…），返回分配的 id。
    QString addFault(const QString &name, const QString &interpreter = QString());
    bool removeFault(const QString &faultId);
    // 改名；与集内其他断层撞名 → false 不变。
    bool renameFault(const QString &faultId, const QString &newName);
    bool setFaultVisible(const QString &faultId, bool visible);
    void clear();

    const QVector<Fault> &faults() const { return m_faults; }
    int faultCount() const { return static_cast<int>(m_faults.size()); }
    const Fault *faultById(const QString &faultId) const;
    // 集内可用名：base 本身空闲则原样返回，否则 "base1/base2…" 递增。
    QString uniqueName(const QString &base) const;
    // 名字是否已被占用（改名预检用；重名在本集内不允许）。
    bool hasFaultNamed(const QString &name) const;
    // id 分配预测（不改动模型）：编排器在入栈前预演命令用的确定性 id。
    int nextFaultId() const { return m_nextFaultId; }
    int nextStickId() const { return m_nextStickId; }
    // 按既有 id 恢复插入（undo 重放/序列化恢复路径）；不查重、不替换，
    // id 计数按需上抬。
    void insertFault(const Fault &fault);

    // ---- 断层棒 ----
    // 追加棒；stick.id 空则分配 "s-N"（全局唯一）。faultId 不存在 → false。
    bool addStick(const QString &faultId, const FaultStick &stick, QString *assignedId = nullptr);
    bool removeStick(const QString &faultId, const QString &stickId);
    // 各断层在指定剖面上的棒（faultId, stick）对，剖面身份 matchKey 匹配。
    QVector<QPair<QString, FaultStick>> sticksForSection(const FaultSectionRef &section) const;

    // ---- 断层-层位切割（独立存取，Oracle #4）----
    // 写入/替换该断层在该层位的切割；faultId 不存在 → false。
    bool setCut(const QString &faultId, const FaultHorizonCut &cut);
    bool removeCut(const QString &faultId, const QString &horizon);
    const FaultHorizonCut *cut(const QString &faultId, const QString &horizon) const;

    // ---- 序列化 ----
    QByteArray toJson() const;
    // 解析失败（非对象/缺 faults 数组/元素形状不符）→ false + *error，本集不变。
    bool fromJson(const QByteArray &json, QString *error = nullptr);

private:
    QVector<Fault> m_faults;
    int m_nextFaultId = 1;
    int m_nextStickId = 1;
};

} // namespace paleo::fault
