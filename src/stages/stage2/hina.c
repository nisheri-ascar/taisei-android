/*
 * This software is licensed under the terms of the MIT License.
 * See COPYING for further information.
 * ---
 * Copyright (c) 2011-2019, Lukas Weber <laochailan@web.de>.
 * Copyright (c) 2012-2019, Andrei Alexeyev <akari@taisei-project.org>.
*/

#include "taisei.h"

#include "hina.h"

#include "global.h"

void stage2_draw_hina_spellbg(Boss *h, int time) {
	SpriteParams sp = { 0 };
	sp.pos.x = VIEWPORT_W/2;
	sp.pos.y = VIEWPORT_H/2;
	sp.scale.both = 0.6;
	sp.shader_ptr = res_shader("sprite_default");
	sp.blend = BLEND_PREMUL_ALPHA;
	sp.sprite = "stage2/spellbg1";
	r_draw_sprite(&sp);
	sp.scale.both = 1;
	sp.blend = BLEND_MOD;
	sp.sprite = "stage2/spellbg2";
	sp.rotation = (SpriteRotationParams) { .angle = time * 5 * DEG2RAD, .vector = { 0, 0, 1 } };
	r_draw_sprite(&sp);

	Animation *fireani = res_anim("fire");
	sp.sprite_ptr = animation_get_frame(fireani, get_ani_sequence(fireani, "main"), global.frames);
	sp.sprite = NULL;
	sp.pos.x = creal(h->pos);
	sp.pos.y = cimag(h->pos);
	sp.scale.both = 1;
	sp.rotation = (SpriteRotationParams) { 0 };
	sp.blend = BLEND_PREMUL_ALPHA;
	r_draw_sprite(&sp);
}

Boss *stage2_spawn_hina(cmplx pos) {
	Boss *hina = create_boss("Kagiyama Hina", "hina", pos);
	boss_set_portrait(hina, "hina", NULL, "normal");
	hina->glowcolor = *RGBA_MUL_ALPHA(0.7, 0.2, 0.3, 0.5);
	hina->shadowcolor = hina->glowcolor;
	return hina;
}
