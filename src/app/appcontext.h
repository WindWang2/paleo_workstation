#pragma once
#include <QObject>
#include <QString>

// app/ — AppContext wires the service graph together (composition root).
// Construct once in main(); owns all services; PaleoMainWindow receives them.
class QgisRuntime;
class QgisCanvasController;
class QgisProjectService;
class QgisLayerService;
class QgisProcessingService;
class QgisEditingService;
class QgisStyleService;
class ToolAvailabilityService;
class SelectionContext;
class PaleoProjectStore;
class LayerManifest;
class PredictionWorkflow;
class ConstraintWorkflow;
class CompositionWorkflow;
class ValidationWorkflow;

class AppContext : public QObject
{
  Q_OBJECT
  public:
    explicit AppContext(const QString &qgisPrefix, QObject *parent = nullptr);
    ~AppContext() override;
    bool ready() const { return m_ready; }

    QgisProjectService *projectSvc() const { return m_projectSvc; }
    QgisLayerService *layerSvc() const { return m_layerSvc; }
    QgisCanvasController *canvasCtl() const { return m_canvasCtl; }
    QgisProcessingService *processingSvc() const { return m_procSvc; }
    QgisEditingService *editingSvc() const { return m_editSvc; }
    QgisStyleService *styleSvc() const { return m_styleSvc; }
    ToolAvailabilityService *toolSvc() const { return m_toolSvc; }
    SelectionContext *selection() const { return m_selection; }
    PaleoProjectStore *store() const { return m_store; }
    LayerManifest *manifest() const { return m_manifest; }
    PredictionWorkflow *predictionWf() const { return m_predictionWf; }
    ConstraintWorkflow *constraintWf() const { return m_constraintWf; }
    CompositionWorkflow *compositionWf() const { return m_compositionWf; }
    ValidationWorkflow *validationWf() const { return m_validationWf; }

  private:
    bool m_ready = false;
    QgisProjectService *m_projectSvc = nullptr;
    QgisLayerService *m_layerSvc = nullptr;
    QgisCanvasController *m_canvasCtl = nullptr;
    QgisProcessingService *m_procSvc = nullptr;
    QgisEditingService *m_editSvc = nullptr;
    QgisStyleService *m_styleSvc = nullptr;
    ToolAvailabilityService *m_toolSvc = nullptr;
    SelectionContext *m_selection = nullptr;
    PaleoProjectStore *m_store = nullptr;
    LayerManifest *m_manifest = nullptr;
    PredictionWorkflow *m_predictionWf = nullptr;
    ConstraintWorkflow *m_constraintWf = nullptr;
    CompositionWorkflow *m_compositionWf = nullptr;
    ValidationWorkflow *m_validationWf = nullptr;
};
