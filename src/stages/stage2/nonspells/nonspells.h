/*
 * This software is licensed under the terms of the MIT License.
 * See COPYING for further information.
 * ---
 * Copyright (c) 2011-2019, Lukas Weber <laochailan@web.de>.
 * Copyright (c) 2012-2019, Andrei Alexeyev <akari@taisei-project.org>.
*/

#pragma once
#include "taisei.h"

#include "boss.h"

void hina_cards1(Boss *h, int time);
void hina_cards2(Boss *h, int time);

DECLARE_EXTERN_TASK_WITH_INTERFACE(stage2_midboss_nonspell_1, BossAttack);
DECLARE_EXTERN_TASK_WITH_INTERFACE(stage2_boss_nonspell_1, BossAttack);
DECLARE_EXTERN_TASK_WITH_INTERFACE(stage2_boss_nonspell_2, BossAttack);
DECLARE_EXTERN_TASK_WITH_INTERFACE(stage2_boss_nonspell_3, BossAttack);
