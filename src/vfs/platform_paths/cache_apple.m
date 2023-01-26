/*
 * This software is licensed under the terms of the MIT License.
 * See COPYING for further information.
 * ---
 * Copyright (c) 2011-2019, Lukas Weber <laochailan@web.de>.
 * Copyright (c) 2012-2019, Andrei Alexeyev <akari@taisei-project.org>.
*/

#include "taisei.h"

#include "platform_paths.h"
#include "util.h"
#include "../syspath_public.h"

// `Rect` clashes with macOS SDK namespace, override here
#define Rect apple_Rect

#include <Foundation/Foundation.h>

const char *vfs_platformpath_cache(void) {
	static char *cached;

	if(!cached) { @autoreleasepool {
		NSArray *array = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES);

		if([array count] > 0) {
			const char *path = [[array objectAtIndex:0] fileSystemRepresentation];

			if(path) {
				cached = vfs_syspath_join_alloc(path, "taisei");
			}
		}
	}}

	return cached;
}
