/*
 * This software is licensed under the terms of the MIT License.
 * See COPYING for further information.
 * ---
 * Copyright (c) 2011-2019, Lukas Weber <laochailan@web.de>.
 * Copyright (c) 2012-2019, Andrei Alexeyev <akari@taisei-project.org>.
 */

#include "taisei.h"

#include "projectile.h"
#include "util.h"
#include "random.h"

typedef struct PPBasicPriv {
	const char *sprite_name;
	Sprite *sprite;
	cmplx size;
	cmplx collision_size;
} PPBasicPriv;

static void pp_basic_preload(ProjPrototype *proto) {
	preload_resource(RES_SPRITE, ((PPBasicPriv*)proto->private)->sprite_name, RESF_PERMANENT);
	// not assigning ->sprite here because it'll block the thread until loaded
}

static void pp_basic_init_projectile(ProjPrototype *proto, Projectile *p) {
	PPBasicPriv *pdata = proto->private;

	p->sprite = pdata->sprite
		? pdata->sprite
		: (pdata->sprite = res_sprite(pdata->sprite_name));

	p->size = pdata->size;
	p->collision_size = pdata->collision_size;
}

#define _PP_BASIC(sprite, width, height, colwidth, colheight) \
	ProjPrototype _pp_##sprite = { \
		.preload = pp_basic_preload, \
		.init_projectile = pp_basic_init_projectile, \
		.private = (&(PPBasicPriv) { \
			.sprite_name = "proj/" #sprite, \
			.size = (width) + (height) * I, \
			.collision_size = (colwidth) + (colheight) * I, \
		}), \
	}, *pp_##sprite = &_pp_##sprite; \

#define PP_BASIC(sprite, width, height, colwidth, colheight) _PP_BASIC(sprite, width, height, colwidth, colheight)
#include "projectile_prototypes/basic.inc.h"

// TODO: customize defaults
#define PP_PLAYER(sprite, width, height) _PP_BASIC(sprite, width, height, 0, 0)
#include "projectile_prototypes/player.inc.h"

static void pp_blast_init_projectile(ProjPrototype *proto, Projectile *p) {
	pp_basic_init_projectile(proto, p);
	assert(p->rule == NULL);
	assert(p->timeout > 0);

	real a1_x, a1_y, a2_x, a2_y;
	a1_x = rng_range(0, 360);
	a1_y = rng_real();
	a2_x = rng_real();
	a2_y = rng_real();

	p->args[1] = CMPLX(a1_x, a1_y);
	p->args[2] = CMPLX(a2_x, a2_y);
}

ProjPrototype _pp_blast = {
	.preload = pp_basic_preload,
	.init_projectile = pp_blast_init_projectile,
	.private = &(PPBasicPriv) {
		.sprite_name = "part/blast",
		.size = 128 * (1+I),
	},
}, *pp_blast = &_pp_blast;
