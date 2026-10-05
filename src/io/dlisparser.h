// 层：数据
#pragma once
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

#include "lasparser.h" // LasHeaderInfo / LasCurve / LasIssue（井曲线文档共用契约）

// io/ — DLIS (RP66 v1, 1991) 测井曲线解析器：纯 Qt、无 QGIS 依赖、无第三方
// 依赖（方向 44「自写受限子集」决策——vendored 首选无成熟方案，见
// .goal-loop-ledger-welllog-fmt.md 依赖对账）。规范条目逐条对账也进 ledger。
//
// 读全约定（goal/44 扩量口径，无逃逸口）：
//   - 井名：第一个逻辑文件 ORIGIN 对象的 WELL-NAME 属性；
//   - 全部曲线目录：全部 CHANNEL 对象（mnemonic=对象标识符，unit=UNITS，
//     descr=LONG-NAME 引用的可读名，能解析到就带上）；
//   - 全帧数据：FDATA（IFLR type 0）逐帧解码，取值进 LasCurve.values；
//   - 单位与深度基准：索引道单位 + FRAME.INDEX-TYPE（BOREHOLE-DEPTH→"MD"，
//     VERTICAL-DEPTH→"TVD"，缺失→""如实未知）。
//
// 输出即 LasHeaderInfo/LasCurve——与 LAS 同一消费面（welllogset/lascache/
// preview 无感格式差异，分派见 welllogread.h）。
//
// 显式不支持子结构（白名单，逐项有因，测试各有夹具证据）：
//   1. 加密逻辑记录（LRS ENCRYPT 位 / FRAME ENCRYPTED=1）——无密钥语义；
//   2. 复数通道（CSINGL/CDOUBL）与多维通道（DIMENSION 元素积>1）——
//      LasCurve 每格一个标量，数组样本没有诚实的一维投影；
//   3. NO-FORMAT 记录（IFLR type 1）——RP66 不定义其内容布局；
//   4. 第二个及以后的逻辑文件（再遇 FHLR 即止）——井曲线并集读面是
//      单井单深度轴表格，多逻辑文件拼接会伪造连续性；
//   5. 帧号乱序/重复——按文件序采值，issue 如实记（不重排不丢帧）。
// 白名单之外的子结构默认必须解析：未知 Set 类型按通用 EFLR 机器走通
// （不解释语义但不失败），解析不了的按错误逐条报因，绝不静默跳过。
//
// 流式口径：SUL/VR/LRS 逐段读文件（QFile 顺序读 + 段缓冲），不整载内存；
// 曲线值本体（QVector<double>）是输出契约，与 LAS parse 同载。

class DlisParser
{
  public:
    // 整文件解析：井名 + 曲线目录 + 全帧数据。失败返回 false 并写 *error
    // （截断/坏段/非法布局逐条给因），已解出的部分仍尽可能填进 out
    // （诚实面：坏在数据体时不抹掉头目录）。issues 记分级告警
    // （Truncated/Format/Io），与 LasParser::parseDoc 同一口径。
    static bool parse(const QString &path, LasHeaderInfo &header,
                      QList<LasCurve> &curves, QString *error = nullptr,
                      QList<LasIssue> *issues = nullptr);

    // 头部快照：只走到首个 FDATA 为止（ORIGIN/CHANNEL/FRAME 均在其前），
    // 不解码帧数据——与 LasParser::parseHeader 的「头几 KB 代价」语义对齐。
    static bool parseHeader(const QString &path, LasHeaderInfo &out,
                            QString *error = nullptr,
                            QList<LasIssue> *issues = nullptr);

    // 内容嗅探：前 200 字节内找到合法 SUL（80 字节 RECORD 结构）→ true。
    // 扩展名不参与判定——扩展名分派在 welllogread 层做。
    static bool sniff(const QString &path);

    // ---- RP66 值类型（EFLR 属性 / 帧槽共用解码面；仅测试与内部使用）----
    struct Obname
    {
        qint64 origin = 0;
        int copy = 0;
        QString id;
    };
};
