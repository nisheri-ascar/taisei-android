/*
 * This software is licensed under the terms of the MIT License.
 * See COPYING for further information.
 * ---
 * Copyright (c) 2011-2019, Lukas Weber <laochailan@web.de>.
 * Copyright (c) 2012-2019, Andrei Alexeyev <akari@taisei-project.org>.
 */

#include "taisei.h"

#include "spellpractice.h"
#include "common.h"
#include "options.h"
#include "global.h"
#include "video.h"

static void draw_spell_menu(MenuData *m) {
	draw_options_menu_bg(m);
	draw_menu_title(m, "Spell Practice");
	draw_menu_list(m, 100, 100, NULL, SCREEN_H, NULL);
}

MenuData* create_spell_menu(void) {
	char title[128];
	Difficulty lastdiff = D_Any;

	MenuData *m = alloc_menu();

	m->draw = draw_spell_menu;
	m->logic = animate_menu_list;
	m->flags = MF_Abortable;
	m->transition = TransFadeBlack;

	int n = stageinfo_get_num_stages();
	for(int i = 0; i < n; ++i) {
		StageInfo *stg = stageinfo_get_by_index(i);

		if(stg->type != STAGE_SPELL) {
			continue;
		}

		if(stg->difficulty < lastdiff || (stg->difficulty == D_Extra && lastdiff != D_Extra)) {
			add_menu_separator(m);
		}

		StageProgress *p = stageinfo_get_progress(stg, D_Any, false);

		if(p && p->unlocked) {
			snprintf(title, sizeof(title), "%s: %s", stg->title, stg->subtitle);
			add_menu_entry(m, title, start_game, stg);
		} else {
			snprintf(title, sizeof(title), "%s: ???????", stg->title);
			add_menu_entry(m, title, NULL, NULL);
		}

		lastdiff = stg->difficulty;
	}

	add_menu_separator(m);
	add_menu_entry(m, "Back", menu_action_close, NULL);

	while(!dynarray_get(&m->entries, m->cursor).action) {
		++m->cursor;
	}

	return m;
}
