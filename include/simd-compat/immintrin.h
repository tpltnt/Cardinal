/*
 * DISTRHO Cardinal Plugin
 * Copyright (C) 2021-2022 Filipe Coelho <falktx@falktx.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 3 of
 * the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * For a full copy of the GNU General Public License see the LICENSE file.
 */

#pragma once

#if defined(__i386__) || defined(__x86_64__) || defined(__EMSCRIPTEN__)
# include_next <immintrin.h>
#else
# define SIMDE_ENABLE_NATIVE_ALIASES
# include "../simde/simde/x86/sse.h"
# include "../simde/simde/x86/sse2.h"
#endif
