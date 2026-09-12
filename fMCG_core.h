// fMCG_core.h -- umbrella header for the fMCG core library.
//
// The implementation lives in focused modules under src/. This header exists
// so existing consumers (test harnesses, the GUI) keep including a single
// file; it does not define anything itself.
#pragma once

#include "src/fmcg_path.h"
#include "src/fmcg_util.h"
#include "src/fmcg_midi.h"
#include "src/fmcg_engine.h"
#include "src/fmcg_format.h"
#include "src/fmcg_fonts.h"
#include "src/fmcg_render.h"
