#pragma once
#include "hw/Types.h"

namespace hw {

// Reads every source. Blocking (pass-through IOCTLs can take a moment on sleeping HDDs): run off the UI thread.
Report Collect();

} // namespace hw
