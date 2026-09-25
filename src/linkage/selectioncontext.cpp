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
  if (m_broadcastDepth > 0)
  {
    // Broadcast in flight — fold this set into the pending slot instead of
    // recursing. Repeated nested sets keep only the latest payload.
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

  // Settle pass: payloads coalesced during the broadcast fire once. The loop
  // covers a settle broadcast that itself triggers another re-set — each
  // generation still emits at most one merged payload.
  while (m_pending)
  {
    m_pending = false;
    m_ids = m_pendingIds;
    m_origin = m_pendingOrigin;
    ++m_broadcastDepth;
    emit selectionChanged(m_ids, m_origin);
    --m_broadcastDepth;
  }
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

  while (m_pending)
  {
    m_pending = false;
    m_ids = m_pendingIds;
    m_origin = m_pendingOrigin;
    ++m_broadcastDepth;
    emit selectionChanged(m_ids, m_origin);
    --m_broadcastDepth;
  }
}

void SelectionContext::clear(const QString &origin)
{
  setSelection(QStringList(), origin);
}
