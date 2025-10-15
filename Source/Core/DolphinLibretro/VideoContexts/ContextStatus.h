#pragma once

enum class ContextState
{
  Unknown,
  Reset,
  Destroyed
};

struct ContextStatus
{
  ContextState state = ContextState::Unknown;

  void MarkReset()     { state = ContextState::Reset; }
  void MarkDestroyed() { state = ContextState::Destroyed; }

  bool IsReady() const     { return state == ContextState::Reset; }
  bool IsDestroyed() const { return state == ContextState::Destroyed; }
};

extern ContextStatus g_context_status;
