/*
 * This software is licensed under the terms of the MIT License.
 * See COPYING for further information.
 * ---
 * Copyright (c) 2011-2019, Lukas Weber <laochailan@web.de>.
 * Copyright (c) 2012-2019, Andrei Alexeyev <akari@taisei-project.org>.
*/

#include "taisei.h"

#include "state.h"
#include "struct.h"
#include "eventcodes.h"

#include "util.h"

void replay_state_init_play(ReplayState *rst, Replay *rpy, ReplayStage *rstage) {
	memset(rst, 0, sizeof(*rst));
	rst->replay = rpy;
	rst->stage = rstage;
	rst->mode = REPLAY_PLAY;
	rst->play.desync_frame = -1;
	rst->play.desync_check_frame = -1;
}

void replay_state_init_record(ReplayState *rst, Replay *rpy) {
	memset(rst, 0, sizeof(*rst));
	rst->replay = rpy;
	rst->mode = REPLAY_RECORD;
}

void replay_state_deinit(ReplayState *rst) {
	memset(rst, 0, sizeof(*rst));
}

ReplaySyncStatus replay_state_check_desync(ReplayState *rst, int time, uint16_t check) {
	if(!rst->stage) {
		return REPLAY_SYNC_NODATA;
	}

	assert(rst->replay != NULL);
	assert(rst->mode == REPLAY_PLAY);

	if(rst->play.desync_check_frame != time) {
		return REPLAY_SYNC_NODATA;
	}

	if(rst->play.desync_check != check) {
		log_warn("Frame %d: replay desync detected! 0x%04x != 0x%04x", time, rst->play.desync_check, check);

		if(rst->play.desync_frame < 0) {
			rst->play.desync_frame = time;
		}

		return REPLAY_SYNC_FAIL;
	}

	log_debug("Frame %d: 0x%04x OK", time, check);
	return REPLAY_SYNC_OK;
}

void replay_state_play_advance(ReplayState *rst, int frame, ReplayEventFunc event_callback, void *arg) {
	assert(rst->mode == REPLAY_PLAY);

	ReplayStage *s = NOT_NULL(rst->stage);
	int nevents = s->events.num_elements;

	int i;
	for(i = rst->play.pos; i < nevents; ++i) {
		ReplayEvent *e = dynarray_get_ptr(&s->events, i);

		if(e->frame != frame) {
			break;
		}

		switch(e->type) {
			case EV_CHECK_DESYNC:
				rst->play.desync_check = e->value;
				rst->play.desync_check_frame = frame;
				break;

			case EV_FPS:
				rst->play.fps = e->value;
				break;

			default:
				event_callback(e, arg);
				break;
		}
	}

	rst->play.pos = i;
}
