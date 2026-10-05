// 层：数据
#pragma once
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

#include "lasparser.h" // LasHeaderInfo / LasCurve / LasIssue（井曲线文档共用契约）

// io/ — LIS79（Log Information Standard，1979/84 子集）测井曲线解析器：
// 纯 Qt、无 QGIS 依赖、无第三方依赖（方向 44「自写受限子集」，规范对账见
// .goal-loop-ledger-welllog-fmt.md；LIS79 官方 PDF 为扫描件，字节布局按
// 规范条文 + dlisio（MPL-2.0）参考实现逐位核准，未抄代码）。
//
// 读全约定（goal/44 扩量口径）：
//   - 井名：Wellsite Data（类型 34）信息记录中助记符 WELL 的字符串分量；
//   - 全部曲线目录：主 logset（首个有数据记录的 DFSR）的全部 Spec Block
//     （mnemonic / units / reprc / samples）；
//   - 全帧数据：Normal Data（类型 0）逐帧解码，深度记录模式 0（帧内索引道）
//     与模式 1（每记录一个深度 + 帧距步进）都支持，UP/DOWN 方向步进符号化；
//   - 单位与深度基准：spec units 原样透出（4 字符去尾空）；基准按
//     subtype-1 过程指示器的 TVD 校正位（"TVD"）否则 "MD"（LIS79 惯例）。
//
// 物理层：TIF（磁带映像，4 字节记录头 [len u16 BE][type u16 BE]）自动识别
// 解包；裸 PR 流也支持。PR 间杂数（0x00/0x20 填充）按双字节前瞻跳过。
// PR 尾（文件号/记录号/校验和）按属性位跳过。流式：逐 PR 读，不整载内存。
//
// 显式不支持子结构（白名单，逐项有因，测试各有夹具证据）：
//   1. 快道（samples>1）logset——子帧布局 V1 未实现，整个 logset 列白名单；
//   2. 字符串/掩码 reprc（65/77）通道——无标量列表示；
//   3. Encrypted/Table Dump（42/47）——无密钥/表布局语义；
//   4. 第二个及以后的 logset（再遇 DFSR）——单井单深度轴表格契约；
//   5. 模式 1 且 UP/DOWN 方向未定义——无法推算深度，不猜。
// 白名单外默认必须解析：未知记录类型按 LIS79 表外如实报错，不静默。

class LisParser
{
  public:
    // 整文件解析：井名 + 主 logset 曲线目录 + 全帧数据。失败 false + *error
    // 给因；issues 分级（Truncated/Format/Io）与 LasParser 同口径。
    static bool parse(const QString &path, LasHeaderInfo &header,
                      QList<LasCurve> &curves, QString *error = nullptr,
                      QList<LasIssue> *issues = nullptr);

    // 头部快照：走到首个数据记录为止（井名/DFSR 均在其前），不解码帧。
    static bool parseHeader(const QString &path, LasHeaderInfo &out,
                            QString *error = nullptr,
                            QList<LasIssue> *issues = nullptr);

    // 内容嗅探：TIF 记录头模式或裸 PRH 模式可完整走通 → true。
    static bool sniff(const QString &path);
};
