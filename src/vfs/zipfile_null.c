/*
 * This software is licensed under the terms of the MIT License.
 * See COPYING for further information.
 * ---
 * Copyright (c) 2011-2019, Lukas Weber <laochailan@web.de>.
 * Copyright (c) 2012-2019, Andrei Alexeyev <akari@taisei-project.org>.
 */

#include "taisei.h"

#include "zipfile.h"

VFSNode *vfs_zipfile_create(VFSNode *source) {
	vfs_set_error("Compiled without ZIP support");
	return NULL;
}
