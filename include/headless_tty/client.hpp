#pragma once

#include <string>

namespace headless_tty {

// Attaches this console to a running session until detach or session end. Returns the child's exit code when the session ended, 0 on detach, 1 on failure.
int run_attach(const std::wstring& name);

}
