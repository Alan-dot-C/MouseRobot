// ============================================================================
//  guard.h - capability guard, spoken announcement, post-action verification
//
//  These three pieces exist for one reason: trust. A tool that can move the
//  mouse and type text is dangerous if it cannot be checked.
// ============================================================================
#pragma once

#include "common.h"
#include <string>

// Returns true when the label looks destructive. `hit` receives the word
// that matched, so the caller can show it.
bool GuardIsDangerous(const std::wstring& text, std::wstring& hit);

// Speak a short line before acting, when --announce is set. No-op otherwise.
void GuardAnnounce(const std::wstring& text);

// After an action, poll the screen until the expectation holds.
//   expectText : text that should appear
//   expectGone : text that should disappear
// At most one of the two is usually given. Returns 0 when satisfied,
// non-zero on timeout.
int GuardVerify(const std::wstring& expectText,
                const std::wstring& expectGone,
                int timeoutMs,
                int scale);
