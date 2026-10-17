#pragma once
#include "hw/Types.h"

namespace hw {

// Reads every source. Blocking (pass-through IOCTLs can take a moment on sleeping HDDs): run off the UI thread.
Report Collect();

// Flags fields of `current` that changed or appeared since `baseline`. Returns the number of flagged fields.
int ApplyDiff(Report& current, const Report& baseline);
void ClearDiff(Report& report);

} // namespace hw
