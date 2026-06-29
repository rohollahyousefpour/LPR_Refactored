#pragma once
// Compatibility shim (lpr_basler only): the facade includes "SettingsManager_.h"
// and uses SettingsManager unqualified.
#include "lpr/config/SettingsManager.h"
using lpr::SettingsManager;
