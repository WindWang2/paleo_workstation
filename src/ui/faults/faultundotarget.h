// 层：视图
#pragma once
#include "ui/edittools/undogrouprouter.h"
#include "workflow/faultinterpretationcontroller.h"

namespace paleo::fault {

inline PaleoUndoTarget undoTargetForFaultStack(FaultEditStack *stack)
{
    const QPointer<FaultEditStack> guard(stack);
    return {
        [guard](bool undo) -> PaleoUndoStep {
            if (!guard)
                return {};
            return undo ? PaleoUndoStep{guard->canUndo(), guard->undoText()}
                        : PaleoUndoStep{guard->canRedo(), guard->redoText()};
        },
        [guard](bool undo) { return guard && (undo ? guard->undo() : guard->redo()); }
    };
}

} // namespace paleo::fault
