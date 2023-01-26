/*
 * This software is licensed under the terms of the MIT License.
 * See COPYING for further information.
 * ---
 * Copyright (c) 2011-2019, Lukas Weber <laochailan@web.de>.
 * Copyright (c) 2012-2019, Andrei Alexeyev <akari@taisei-project.org>.
 */

#pragma once
#include "taisei.h"

#if __has_attribute(analyzer_noreturn)
	#define attr_analyzer_noreturn \
		__attribute__ ((analyzer_noreturn))
#else
	#define attr_analyzer_noreturn
#endif

attr_analyzer_noreturn
void _ts_assert_fail(const char *cond, const char *func, const char *file, int line, bool use_log);

#undef assert
#undef static_assert

#define static_assert _Static_assert
#define static_assert_nomsg(x) static_assert(x, #x)

#if defined(__EMSCRIPTEN__)
	void _emscripten_trap(void);
	#define TRAP() _emscripten_trap()
#elif defined(NDEBUG)
	#define TRAP() abort()
#elif defined(__clang__)
	#define TRAP() __builtin_debugtrap()
#elif defined(__GNUC__)
	#define TRAP() __builtin_trap()
#elif TAISEI_BUILDCONF_HAVE_POSIX
	#include <signal.h>
	#define TRAP() raise(SIGTRAP)
#else
	#define TRAP() abort()
#endif

#ifdef NDEBUG
    #define _assert(cond, uselog)
	#define _assume(cond, uselog) ASSUME(cond)
#else
    #define _assert(cond, uselog) (LIKELY(cond) ? (void)0 : (_ts_assert_fail(#cond, __func__, _TAISEI_SRC_FILE, __LINE__, uselog), TRAP()))
	#define _assume(cond, uselog) _assert(cond, uselog)
#endif

#define assert(cond) _assert(cond, true)
#define assert_nolog(cond) _assert(cond, false)

#define assume(cond) _assume(cond, true)
#define assume_nolog(cond) _assume(cond, false)
