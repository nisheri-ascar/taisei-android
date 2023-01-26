/*
 * This software is licensed under the terms of the MIT License.
 * See COPYING for further information.
 * ---
 * Copyright (c) 2011-2019, Lukas Weber <laochailan@web.de>.
 * Copyright (c) 2012-2019, Andrei Alexeyev <akari@taisei-project.org>.
*/

#include "taisei.h"

#include "misc.h"

Projectile *stage1_spawn_stain(cmplx pos, float angle, int to) {
	return PARTICLE(
		.sprite = "stain",
		.pos = pos,
		.draw_rule = pdraw_timeout_scalefade(0, 0.8, 1, 0),
		.timeout = to,
		.angle = angle,
		.color = RGBA(0.4, 0.4, 0.4, 0),
	);
}
