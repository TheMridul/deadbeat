#pragma once

#include <QStringList>

// The commands that need no window: what agents and the systemd timer use.
// Each takes --json, exits 0 or 1, and writes errors to stderr.
namespace Cli {

// True when the arguments ask for the window (none, or `open`).
bool wantsWindow(const QStringList &args);
int run(const QStringList &args);

} // namespace Cli
