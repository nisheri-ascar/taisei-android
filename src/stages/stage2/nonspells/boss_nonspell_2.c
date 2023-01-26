/*
 * This software is licensed under the terms of the MIT License.
 * See COPYING for further information.
 * ---
 * Copyright (c) 2011-2019, Lukas Weber <laochailan@web.de>.
 * Copyright (c) 2012-2019, Andrei Alexeyev <akari@taisei-project.org>.
*/

#include "taisei.h"

#include "nonspells.h"

#include "global.h"
#include "common_tasks.h"

TASK(speen, { BoxedBoss boss; }) {
	Boss *boss = TASK_BIND(ARGS.boss);
	aniplayer_queue(&boss->ani, "guruguru", 0);

	boss->move = (MoveParams) { 0 };
	cmplx opos = boss->pos;

	real trate = difficulty_value(0.6, 0.8, 0.9, 1.0);

	for(int t = 0;; t += WAIT(1)) {
		real phase = t * trate;
		cmplx target = VIEWPORT_W/2 + 150*I + cwmul(cdir(phase*M_TAU/150), 200 + 42*I);
		boss->pos = clerp(opos, target, smoothmin(phase / 300.0, 1.0, 0.5));
	}
}

TASK(balls, { BoxedBoss boss; }) {
	Boss *boss = TASK_BIND(ARGS.boss);

	for(;;) {
		WAIT(75);
		play_sfx("shot_special1");
		play_sfx("redirect");

		cmplx aim = cnormalize(global.plr.pos - boss->pos);

		int n = 12;
		for(int i = 0; i < n; ++i) {
			cmplx d = aim * cdir(i * M_TAU / n);

			PROJECTILE(
				.proto = pp_bigball,
				.pos = boss->pos,
				.color = RGB(0.8, 0.0, 0.8),
				.move = move_asymptotic_halflife(8 * d, 3 * d, 60),
			);

			int m = 8;
			for(int j = 0; j < m; ++j) {
				real s = (m - j) / (real)m;
				PROJECTILE(
					.proto = pp_ball,
					.pos = boss->pos,
					.color = j & 1 ? RGBA(0.8, 0.0, 0.0, 0.0) : RGBA(0.0, 0.0, 0.8, 0.0),
					.move = move_asymptotic_halflife((8 - 3 * s) * d, 3 * d, 60),
				);
			}
		}
	}
}

DEFINE_EXTERN_TASK(stage2_boss_nonspell_2) {
	STAGE_BOOKMARK(boss-non2);

	Boss *boss = INIT_BOSS_ATTACK(&ARGS);
	boss->move = move_towards(VIEWPORT_W/2 + 100*I, 0.01);
	BEGIN_BOSS_ATTACK(&ARGS);

	INVOKE_SUBTASK(speen, ENT_BOX(boss));
	INVOKE_SUBTASK_DELAYED(150, balls, ENT_BOX(boss));

	int step = difficulty_value(3, 2, 1, 1);
	real speed0 = difficulty_value(1.5, 2.0, 2.8, 3.2);
	real speed1 = 0.2; // difficulty_value(0.1, 0.1, 0.1, 0.2);

	for(int t = 0;; t += WAIT(step)) {
		// play_sfx_ex("shot1", 4, false);
		play_sfx_loop("shot1_loop");

		cmplx dir = cdir(t / 5.0);
		cmplx ofs = 50 * cdir(t / 10.0);

		real hl = 30 + 50 * psin(t/60.0);

		PROJECTILE(
			.proto = pp_card,
			.pos = boss->pos + ofs,
			.color = RGB(0.8, 0.0, 0.0),
			.move = move_asymptotic_halflife(speed1 * dir, speed0 * dir * 2, hl),
		);

		PROJECTILE(
			.proto = pp_card,
			.pos = boss->pos - ofs,
			.color = RGB(0.0, 0.0, 0.8),
			.move = move_asymptotic_halflife(speed1 * -dir, speed0 * -dir * 2, hl),
		);
	}

	STALL;
}
