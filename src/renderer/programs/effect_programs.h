// Aggregate include for built-in effect/compositor/palette programs.
// Implementations live one-program-per-file; this header exists only so the
// registry can import the complete built-in effect inventory from one place.
#pragma once

#include "renderer/programs/alpha_mult_program.h"
#include "renderer/programs/blurh_program.h"
#include "renderer/programs/blurv_program.h"
#include "renderer/programs/color_mult_program.h"
#include "renderer/programs/comp_fade_program.h"
#include "renderer/programs/crt_program.h"
#include "renderer/programs/default_compositor_program.h"
#include "renderer/programs/fxaa_program.h"
#include "renderer/programs/gblurh_program.h"
#include "renderer/programs/gblurv_program.h"
#include "renderer/programs/img_fade_program.h"
#include "renderer/programs/outline_program.h"
#include "renderer/programs/palette_program.h"
#include "renderer/programs/pixilation_program.h"
