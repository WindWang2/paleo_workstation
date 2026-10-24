// 层：功能
#include "selectioncontext.h"

// §41.3 — the single selection broadcast hub.
//
// Re-entrancy / coalescing: every emission is wrapped in m_broadcastDepth.
// A setSelection() arriving while a broadcast is in flight does not re-enter
// the emit — its payload merges into the pending slot (last writer wins: a
// selection is absolute state, so the most recent call describes it) and
// fires once as a coalesced re-broadcast when the outer emission settles.

SelectionContext::SelectionContext(QObject *parent) : QObject(parent) {
  qRegisterMetaType<SelectionTarget>();
}

void SelectionContext::setTarget(const SelectionTarget &target, const QString &origin)
{
  // A late response for another horizon must not replace the current target.
  if (!target.horizon.isEmpty() && target.horizon != m_horizon)
    return;
  if (m_target == target && !m_targetPending)
    return; // origins identify producers; they do not change business selection
  if (m_broadcastDepth > 0) {
    m_pendingTarget = target;
    m_pendingTargetOrigin = origin;
    m_targetPending = true;
    return;
  }
  m_target = target;
  m_targetOrigin = origin;
  ++m_targetGeneration;
  ++m_broadcastDepth;
  emit targetChanged(m_target, m_targetOrigin, m_targetGeneration);
  --m_broadcastDepth;
  settlePending("setTarget");
}

void SelectionContext::clearTarget(const QString &origin)
{
  setTarget({}, origin);
}

void SelectionContext::setSelection(const QStringList &ids, const QString &origin)
{
  if (m_broadcastDepth > 0)
  {
    // Broadcast in flight — fold this set into the pending slot instead of
    // recursing. Repeated nested sets keep only the latest payload.
    // PW-062: a nested set that reverts to the ids being broadcast cancels any
    // earlier pending payload (true last-writer-wins) instead of being dropped.
    if (m_ids == ids)
    {
      m_pending = false;
      m_pendingIds.clear();
      m_pendingOrigin.clear();
      return;
    }
    // Same payload already pending: ignore to prevent infinite echo loops.
    if (m_pending && m_pendingIds == ids)
      return;
    m_pending = true;
    m_pendingIds = ids;
    m_pendingOrigin = origin;
    return;
  }

  if (m_ids == ids && m_origin == origin)
    return;

  m_ids = ids;
  m_origin = origin;
  ++m_broadcastDepth;
  emit selectionChanged(m_ids, m_origin);
  --m_broadcastDepth;

  settlePending("setSelection");
}

void SelectionContext::setActiveHorizon(const QString &horizon)
{
  if (m_horizon == horizon && !m_horizonPending)
    return;
  if (m_broadcastDepth > 0) {
    m_pendingHorizon = horizon;
    m_horizonPending = true;
    return;
  }
  broadcastHorizon(horizon);
  settlePending("setActiveHorizon");
}

void SelectionContext::broadcastHorizon(const QString &horizon)
{
  if (m_horizon == horizon) return;
  m_horizon = horizon;
  ++m_broadcastDepth;
  // Clear spatial targets before exposing the new horizon. Keep legacy wells,
  // and fold any target/horizon callbacks into the same bounded settle loop.
  if (!m_target.horizon.isEmpty() && m_target.horizon != horizon) {
    m_target = {};
    m_targetOrigin = QStringLiteral("horizon");
    ++m_targetGeneration;
    emit targetChanged(m_target, m_targetOrigin, m_targetGeneration);
  }
  if (m_targetPending && !m_pendingTarget.horizon.isEmpty() &&
      m_pendingTarget.horizon != horizon)
    m_targetPending = false;

  // Same depth guard: a slot that calls setSelection() from here coalesces
  // into the pending slot rather than re-entering; it is settled below.
  emit activeHorizonChanged(m_horizon);
  --m_broadcastDepth;
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
  while ((m_pending || m_targetPending || m_horizonPending) && settleGenerations < kMaxSettleGenerations)
  {
    ++settleGenerations;
    if (m_horizonPending) {
      m_horizonPending = false;
      broadcastHorizon(m_pendingHorizon);
    }
    if (m_pending) {
      m_pending = false;
      if (m_ids != m_pendingIds || m_origin != m_pendingOrigin) {
        m_ids = m_pendingIds;
        m_origin = m_pendingOrigin;
        ++m_broadcastDepth;
        emit selectionChanged(m_ids, m_origin);
        --m_broadcastDepth;
      }
    }
    if (m_targetPending) {
      m_targetPending = false;
      if ((m_pendingTarget.horizon.isEmpty() || m_pendingTarget.horizon == m_horizon) &&
          m_target != m_pendingTarget) {
        m_target = m_pendingTarget;
        m_targetOrigin = m_pendingTargetOrigin;
        ++m_targetGeneration;
        ++m_broadcastDepth;
        emit targetChanged(m_target, m_targetOrigin, m_targetGeneration);
        --m_broadcastDepth;
      }
    }
  }
  if (m_pending || m_targetPending || m_horizonPending)
  {
    qWarning("SelectionContext::%s: settle iteration limit reached, dropped runaway pending selection", caller);
    m_pending = false;
    m_targetPending = false;
    m_horizonPending = false;
  }
}
