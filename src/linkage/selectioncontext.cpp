// 层：功能
#include "selectioncontext.h"

// §41.3 — the single selection broadcast hub.
//
// Re-entrancy / coalescing: every emission is wrapped in m_broadcastDepth.
// A setSelection() arriving while a broadcast is in flight does not re-enter
// the emit — its payload merges into the pending slot (last writer wins: a
// selection is absolute state, so the most recent call describes it) and
// fires once as a coalesced re-broadcast when the outer emission settles.

SelectionContext::SelectionContext(QObject *parent) : QObject(parent) {}

void SelectionContext::setSelection(const QStringList &ids, const QString &origin)
{
  if (m_ids == ids && m_origin == origin)
    return;

  if (m_broadcastDepth > 0)
  {
    // Broadcast in flight — fold this set into the pending slot instead of
    // recursing. Repeated nested sets keep only the latest payload.
    // If the payload matches current or already pending broadcast, ignore to prevent infinite echo loops.
    if (m_ids == ids || (m_pending && m_pendingIds == ids))
      return;
    m_pending = true;
    m_pendingIds = ids;
    m_pendingOrigin = origin;
    return;
  }

  m_ids = ids;
  m_origin = origin;
  ++m_broadcastDepth;
  emit selectionChanged(m_ids, m_origin);
  --m_broadcastDepth;

  settlePending("setSelection");
}

void SelectionContext::setActiveHorizon(const QString &horizon)
{
  if (m_horizon == horizon)
    return;
  m_horizon = horizon;

  // Same depth guard: a slot that calls setSelection() from here coalesces
  // into the pending slot rather than re-entering; it is settled below.
  ++m_broadcastDepth;
  emit activeHorizonChanged(m_horizon);
  --m_broadcastDepth;

  settlePending("setActiveHorizon");
}

void SelectionContext::clear(const QString &origin)
{
  setSelection(QStringList(), origin);
}

void SelectionContext::settlePending(const char *caller)
{
  // Settle pass: payloads coalesced during the broadcast fire once. The loop
  // covers a settle broadcast that itself triggers another re-set — each
  // generation still emits at most one merged payload. Bound at 4 iterations.
  int settleGenerations = 0;
  constexpr int kMaxSettleGenerations = 4;
  while (m_pending && settleGenerations < kMaxSettleGenerations)
  {
    ++settleGenerations;
    m_pending = false;
    if (m_ids == m_pendingIds && m_origin == m_pendingOrigin)
      break;
    m_ids = m_pendingIds;
    m_origin = m_pendingOrigin;
    ++m_broadcastDepth;
    emit selectionChanged(m_ids, m_origin);
    --m_broadcastDepth;
  }
  if (m_pending)
  {
    qWarning("SelectionContext::%s: settle iteration limit reached, dropped runaway pending selection", caller);
    m_pending = false;
  }
}
