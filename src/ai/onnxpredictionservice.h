// 层：功能
#pragma once
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

// ai/ — PaleoOnnxService wraps the vendored ONNX Runtime C++ API (ET4 spike
// proven: onnxruntime-linux-x64-1.30.0 under vendor/onnxruntime).
// In-process inference only; model files pinned per-horizon under models/.
// First output of an in-process inference. `values` is empty on failure.
struct OnnxTensor {
  QVector<float> values;
  QVector<int64_t> shape;
};

// 失败分类（goal/ai-geological-assist 会话管理）：ORT 异常按语义归类，
// 原始信息保留在错误链里（不吞）。NotFound 在进 ORT 前自判。
enum class OnnxLoadStatus {
  Ok,             // 会话可用（warmup 可能仍失败，见 OnnxModelMeta::warmupError）
  NotFound,       // 模型文件不存在
  BadFormat,      // 不是合法 onnx（protobuf 解析失败）
  UnsupportedIr,  // IR 版本高于运行时支持
  CreateFailed,   // 其它会话构造失败（图不合法/算子缺失/内存…）
};

// 模型指纹 + 输入签名（懒加载后一次取全；池命中直接复用）。
struct OnnxModelMeta {
  QString name;
  QString path;
  QString sha256;             // 文件字节 SHA256（指纹校验/注册表钉哈希用）
  QString inputName;          // 首输入名
  QVector<int64_t> inputShape;// 首输入维；-1 = 动态维
  int inputElemType = 1;      // ONNX TensorProto 元素类型（1 = float32）
  QString inputSignature;     // "x:float32[1,1,?,?]"（动态维记 ?）
  qint64 warmupMs = -1;       // 首次加载 warmup 推理耗时；-1 = 未执行
  QString warmupError;        // warmup 失败原始错误（空 = 成功）
};

class PaleoOnnxService : public QObject
{
  Q_OBJECT
  public:
    explicit PaleoOnnxService(QObject *parent = nullptr);
    ~PaleoOnnxService() override;

    void setModelRoot(const QString &dir);         // models/<name>.onnx
    QStringList availableModels() const;            // *.onnx basenames
    // 兼容原签名；内部走会话池（同指纹二次加载瞬时命中）。
    bool loadModel(const QString &name, QString *error = nullptr);
    // 详细版：成功时 *meta 填指纹/输入签名/warmup 计时。
    OnnxLoadStatus loadModelMeta(const QString &name, OnnxModelMeta *meta, QString *error = nullptr);
    QString loadedModel() const;
    bool isModelLoaded(const QString &name) const;

    // 最近一次 loadModel/Meta 的分类结果与元数据（未加载过 → NotFound/空 meta）。
    OnnxLoadStatus lastLoadStatus() const;
    OnnxModelMeta loadedModelMeta() const;
    bool lastLoadPoolHit() const;   // 最近一次成功加载是否池命中（未重建会话）
    int sessionPoolSize() const;    // 池内活会话数（LRU 上限 4）
    int sessionPoolCapacity() const { return 4; }

    // Toy/general inference: named input float tensor in -> first output float tensor out.
    // Deterministic: same input → same output (spike verified 2.0 → 42.0).
    // 线程安全：会话 Run 与加载/换模互斥（异步 tile 推理从任务池线程调）。
    QVector<float> run(const QString &inputName, const QVector<float> &input,
                       const QVector<int64_t> &shape, QString *error = nullptr);

    // Same as run(), plus the output tensor's shape so callers can write a grid.
    OnnxTensor runTensor(const QString &inputName, const QVector<float> &input,
                         const QVector<int64_t> &shape, QString *error = nullptr);

    static QString vendorRuntimeDir();              // vendor/onnxruntime resolved path
    static bool runtimeAvailable();                 // libonnxruntime found in vendor tree
    // 分块读文件算 SHA256（模型指纹/注册表校验共用；大模型不整读进内存）。
    static QString sha256OfFile(const QString &path);

  signals:
    void modelLoaded(const QString &name);
    void inferenceFailed(const QString &model, const QString &error);

  private:
    QString normalizedModelPath(const QString &name) const;
    struct PoolEntry;                               // Ort::Session* + meta + LRU 戳（cpp 内定义）
    QString m_modelRoot;
    QString m_loaded;
    QString m_loadedPath;
    PoolEntry *m_active = nullptr;                  // 指向池内条目（池拥有会话）
    QHash<QString, PoolEntry *> m_pool;             // key = canonical path
    quint64 m_lruCounter = 0;
    OnnxLoadStatus m_lastStatus = OnnxLoadStatus::NotFound;
    OnnxModelMeta m_lastMeta;
    bool m_lastPoolHit = false;
    mutable QMutex m_mutex;                         // 会话换装/池/Run 全互斥
};
