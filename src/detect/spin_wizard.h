#pragma once

#include "common/ffdata.h"

typedef enum FFSpinWizardResult {
    FF_SPIN_WIZARD_SKIP = 0,
    FF_SPIN_WIZARD_CONTINUE = 1, // first run: configuration applied, fetch may continue
    FF_SPIN_WIZARD_EXIT = 2,     // aborted, or --configure finished
    FF_SPIN_WIZARD_ERROR = 3,    // invalid combination or I/O failure
} FFSpinWizardResult;

FFSpinWizardResult ffSpinWizardMaybeRun(FFdata* data);
