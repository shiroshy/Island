#pragma once
#include <windows.h>
namespace startup {
bool enabled();
bool setEnabled(bool value);
// Migrate only an already enabled registration owned by this exact installation.
HRESULT prepareEarly();
}
