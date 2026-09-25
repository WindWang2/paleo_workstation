#pragma once
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

class PaleoOnnxService : public QObject
{
  Q_OBJECT
  public:
    explicit PaleoOnnxService(QObject *parent = nullptr);
    ~PaleoOnnxService() override;

    void setModelRoot(const QString &dir);         // models/<name>.onnx
    QStringList availableModels() const;            // *.onnx basenames
    bool loadModel(const QString &name, QString *error = nullptr);
    QString loadedModel() const;

    // Toy/general inference: named input float tensor in -> first output float tensor out.
    // Deterministic: same input → same output (spike verified 2.0 → 42.0).
    QVector<float> run(const QString &inputName, const QVector<float> &input,
                       const QVector<int64_t> &shape, QString *error = nullptr);

    // Same as run(), plus the output tensor's shape so callers can write a grid.
    OnnxTensor runTensor(const QString &inputName, const QVector<float> &input,
                         const QVector<int64_t> &shape, QString *error = nullptr);

    static QString vendorRuntimeDir();              // vendor/onnxruntime resolved path
    static bool runtimeAvailable();                 // libonnxruntime found in vendor tree

  signals:
    void modelLoaded(const QString &name);
    void inferenceFailed(const QString &model, const QString &error);

  private:
    QString m_modelRoot;
    QString m_loaded;
    void *m_session = nullptr;                      // Ort::Session* opaque (header stays ORT-free)
};
