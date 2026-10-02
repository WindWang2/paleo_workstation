// 层：功能
#pragma once
#include <QString>
#include <QVector>

// ai/ — 微型 .onnx fixture 生成器：手写 ONNX protobuf 字节（ModelProto/
// GraphProto/NodeProto/TensorProto 直排），零网络、零 python onnx 依赖
//（goal/ai-geological-assist Oracle 1：管线正确性验证，非真实精度）。
// 同参数重复生成字节一致（确定性）；opset 13 + IR 8（ORT 1.30 接受 IR<=12）。
class OnnxFixtureWriter
{
  public:
    // y = x + addend，float32[1] 标量（addend=40 复刻 spikes toy 语义：
    // 输入 2.0 → 42.0）。irVersion 可抬高到运行时支持之上（如 9999）以
    // 制造「版本不兼容」失败分类夹具。
    static bool writeAddScalar( const QString &path, float addend = 40.0f,
                                int64_t irVersion = 8, QString *error = nullptr );

    // NCHW 分类 logits：y[0,c,h,w] = gains[c] * x[0,0,h,w] + biases[c]。
    // 输入 x float32[1,1,H,W]（H/W 动态维），输出 y float32[1,C,H,W]。
    // Mul(w[1,C,1,1]) + Add(b[1,C,1,1]) 两节点——softmax/熵由调用侧计算。
    static bool writeSeg( const QString &path, const QVector<float> &gains,
                          const QVector<float> &biases, int64_t irVersion = 8,
                          QString *error = nullptr );

    // trace 窗评分：p = sigmoid(gain*x + offset)，float32[1,1,T]（T 动态）。
    // Mul + Add + Sigmoid 三节点——逐样本「层位敏感度」概率，供种子点
    // 局部追踪建议打分。
    static bool writeTraceScorer( const QString &path, float gain, float offset,
                                  int64_t irVersion = 8, QString *error = nullptr );
};
