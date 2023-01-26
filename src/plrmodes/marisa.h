/*
 * This software is licensed under the terms of the MIT License.
 * See COPYING for further information.
 * ---
 * Copyright (c) 2011-2019, Lukas Weber <laochailan@web.de>.
 * Copyright (c) 2012-2019, Andrei Alexeyev <akari@taisei-project.org>.
 */

#pragma once
#include "taisei.h"

#include "plrmodes.h"
#include "marisa_a.h"
#include "marisa_b.h"
#include "dialog/marisa.h"
#include "color.h"

extern PlayerCharacter character_marisa;

typedef struct MarisaBeamInfo {
	cmplx origin;
	cmplx size;
	float angle;
	int t;
} MarisaBeamInfo;

double marisa_common_property(Player *plr, PlrProperty prop);
void marisa_common_masterspark_draw(int numBeams, MarisaBeamInfo *beamInfos, float alpha);

DECLARE_EXTERN_TASK(marisa_common_shot_forward, {
	BoxedPlayer plr;
	real damage;
	int delay;
});
