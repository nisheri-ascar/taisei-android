/*
 * This software is licensed under the terms of the MIT License.
 * See COPYING for further information.
 * ---
 * Copyright (c) 2011-2019, Lukas Weber <laochailan@web.de>.
 * Copyright (c) 2012-2019, Andrei Alexeyev <akari@taisei-project.org>.
 */

#include "taisei.h"

#include <stdio.h>

#include "menu.h"
#include "common.h"
#include "options.h"
#include "mainmenu.h"
#include "global.h"
#include "video.h"

typedef struct OptionBinding OptionBinding;

typedef int (*BindingGetter)(OptionBinding*);
typedef int (*BindingSetter)(OptionBinding*, int);
typedef bool (*BindingDependence)(void);

typedef enum BindingType {
	BT_IntValue,
	BT_KeyBinding,
	BT_StrValue,
	BT_Resolution,
	BT_FramebufferResolution,
	BT_Scale,
	BT_GamepadKeyBinding,
	BT_GamepadAxisBinding,
	BT_GamepadDevice,
	BT_VideoDisplay,
} BindingType;

typedef struct OptionBinding {
	union {
		char **values;
		char *strvalue;
	};
	bool displaysingle;
	int valrange_min;
	int valrange_max;
	float scale_min;
	float scale_max;
	float scale_step;
	BindingGetter getter;
	BindingSetter setter;
	BindingDependence dependence;
	int selected;
	int configentry;
	BindingType type;
	bool blockinput;
	int pad;
} OptionBinding;

// --- Menu entry <-> config option binding stuff --- //

static OptionBinding* bind_new(void) {
	return ALLOC(OptionBinding, {
		.selected = -1,
		.configentry = -1,
	});
}

static void bind_free(OptionBinding *bind) {
	int i;

	if(bind->type == BT_StrValue) {
		mem_free(bind->strvalue);
	} else if(bind->values) {
		assert(bind->valrange_min == 0);
		for(i = 0; i <= bind->valrange_max; ++i) {
			mem_free(*(bind->values+i));
		}
		mem_free(bind->values);
	}
}

static OptionBinding* bind_get(MenuData *m, int idx) {
	MenuEntry *e = dynarray_get_ptr(&m->entries, idx);
	return e->arg == m? NULL : e->arg;
}

// BT_IntValue: integer and boolean options
// Values are defined with bind_addvalue or bind_setrange
static OptionBinding* bind_option(int cfgentry, BindingGetter getter, BindingSetter setter) {
	OptionBinding *bind = bind_new();
	bind->type = BT_IntValue;
	bind->configentry = cfgentry;

	bind->getter = getter;
	bind->setter = setter;

	return bind;
}

// BT_KeyBinding: keyboard action mapping options
static OptionBinding* bind_keybinding(int cfgentry) {
	OptionBinding *bind = bind_new();

	bind->configentry = cfgentry;
	bind->type = BT_KeyBinding;

	return bind;
}

// BT_GamepadKeyBinding: gamepad action mapping options
static OptionBinding* bind_gpbinding(int cfgentry) {
	OptionBinding *bind = bind_new();

	bind->configentry = cfgentry;
	bind->type = BT_GamepadKeyBinding;

	return bind;
}

// BT_GamepadAxisBinding: gamepad axis mapping options
static OptionBinding* bind_gpaxisbinding(int cfgentry) {
	OptionBinding *bind = bind_new();

	bind->configentry = cfgentry;
	bind->type = BT_GamepadAxisBinding;

	return bind;
}

static int bind_gpdev_get(OptionBinding *b) {
	const char *guid = config_get_str(b->configentry);
	int val = gamepad_device_num_from_guid(guid);

	if(val == GAMEPAD_DEVNUM_ANY) {
		val = -1;
	}

	return val;
}

static int bind_gpdev_set(OptionBinding *b, int v) {
	char guid[33] = {0};

	if(v == -1) {
		v = GAMEPAD_DEVNUM_ANY;
	}

	gamepad_device_guid(v, guid, sizeof(guid));

	if(*guid) {
		config_set_str(b->configentry, guid);

		if(v == GAMEPAD_DEVNUM_ANY) {
			v = -1;
		}

		b->selected = v;
	}

	return b->selected;
}

#ifndef __SWITCH__
// BT_GamepadDevice: dynamic device list
static OptionBinding* bind_gpdevice(int cfgentry) {
	OptionBinding *bind = bind_new();

	bind->configentry = cfgentry;
	bind->type = BT_GamepadDevice;

	bind->getter = bind_gpdev_get;
	bind->setter = bind_gpdev_set;

	bind->valrange_min = -1;
	bind->valrange_max = 0; // updated later

	bind->selected = gamepad_device_num_from_guid(config_get_str(bind->configentry));

	return bind;
}

// BT_StrValue: with a half-assed "textbox"
static OptionBinding* bind_stroption(ConfigIndex cfgentry) {
	OptionBinding *bind = bind_new();
	bind->type = BT_StrValue;
	bind->configentry = cfgentry;
	stralloc(&bind->strvalue, config_get_str(cfgentry));

	return bind;
}
#endif

// BT_Resolution: super-special binding type for the resolution setting
static void bind_resolution_update(OptionBinding *bind) {
	// FIXME This is brittle. The least we could do is to explicitly store whether the currently selected value represents a fullscreen index or windowed.

	bool fullscreen = video_is_fullscreen();
	uint nmodes = video_get_num_modes(fullscreen);
	VideoMode cur = video_get_current_mode();

	log_debug("Fullscreen: %i", fullscreen);
	log_debug("Prev selected: %i", bind->selected);

	bind->valrange_min = 0;
	bind->valrange_max = nmodes - 1;
	bind->selected = -1;

	log_debug("nmodes = %i", nmodes);

	for(int i = 0; i < nmodes; ++i) {
		VideoMode m = video_get_mode(i, fullscreen);
		log_debug("#%i   %ix%i", i, m.width, m.height);
		if(m.width == cur.width && m.height == cur.height) {
			bind->selected = i;
			log_debug("selected #%i", bind->selected);
		}
	}
}

static OptionBinding* bind_resolution(void) {
	OptionBinding *bind = bind_new();
	bind->type = BT_Resolution;
	bind->selected = -1;
	bind_resolution_update(bind);
	return bind;
}

// BT_FramebufferResolution: not an actual setting (yet); just display effective resolution in pixels
// This may be different from BT_Resolution in the high-DPI case
// (BT_Resolution is a misnomer; it represents the window size in screen-space units)
static OptionBinding* bind_fb_resolution(void) {
	OptionBinding *bind = bind_new();
	bind->type = BT_FramebufferResolution;
	return bind;
}

// BT_Scale: float values clamped to a range
static OptionBinding* bind_scale(int cfgentry, float smin, float smax, float step) {
	OptionBinding *bind = bind_new();
	bind->type = BT_Scale;
	bind->configentry = cfgentry;

	bind->scale_min  = smin;
	bind->scale_max  = smax;
	bind->scale_step = step;

	return bind;
}

// Returns a pointer to the first found binding that blocks input. If none found, returns NULL.
static OptionBinding* bind_getinputblocking(MenuData *m) {
	dynarray_foreach_idx(&m->entries, int i, {
		OptionBinding *bind = bind_get(m, i);
		if(bind && bind->blockinput) {
			return bind;
		}
	});
	return NULL;
}

// Adds a value to a BT_IntValue type binding
static int bind_addvalue(OptionBinding *b, char *val) {
	assert(b->valrange_min == 0);

	if(b->values == NULL) {
		b->values = mem_alloc(sizeof(char*));
		b->valrange_min = 0;
		b->valrange_max = 0;
	} else {
		assert(b->valrange_min == 0);
		++b->valrange_max;
	}

	b->values = mem_realloc(b->values, (1 + b->valrange_max) * sizeof(char*));
	b->values[b->valrange_max] = strdup(val);
	return b->valrange_max;
}

attr_unused
static void bind_setvaluerange(OptionBinding *b, int vmin, int vmax) {
	assert(b->values == NULL);
	b->valrange_min = vmin;
	b->valrange_max = vmax;
}

// Called to select a value of a BT_IntValue type binding by index
static int bind_setvalue(OptionBinding *b, int v) {
	if(b->setter)
		return b->selected = b->setter(b, v);
	else
		return b->selected = v;
}

// Called to get the selected value of a BT_IntValue type binding by index
static int bind_getvalue(OptionBinding *b) {
	if(b->getter) {
		b->selected = b->getter(b);
	}

	return b->selected;
}

// Selects the next to current value of a BT_IntValue type binding
static int bind_setnext(OptionBinding *b) {
	int s = b->selected + 1;

	if(s > b->valrange_max) {
		s = b->valrange_min;
	}

	return bind_setvalue(b, s);
}

// Selects the previous to current value of a BT_IntValue type binding
static int bind_setprev(OptionBinding *b) {
	int s = b->selected - 1;

	if(s < b->valrange_min) {
		s = b->valrange_max;
	}

	return bind_setvalue(b, s);
}

static bool bind_isactive(OptionBinding *b) {
	if(!b->dependence)
		return true;
	return b->dependence();
}

// --- Shared binding callbacks --- //

static int bind_common_onoff_get(OptionBinding *b) {
	return !config_get_int(b->configentry);
}

static int bind_common_onoff_set(OptionBinding *b, int v) {
	return !config_set_int(b->configentry, !v);
}

static int bind_common_onoff_inverted_get(OptionBinding *b) {
	return config_get_int(b->configentry);
}

static int bind_common_onoff_inverted_set(OptionBinding *b, int v) {
	return config_set_int(b->configentry, v);
}

static int bind_common_onoffplus_get(OptionBinding *b) {
	int v = config_get_int(b->configentry);

	if(v > 1)
		return v;
	return !v;
}

static int bind_common_onoffplus_set(OptionBinding *b, int v) {
	if(v > 1)
		return config_set_int(b->configentry, v);
	return !config_set_int(b->configentry, !v);
}

#define bind_common_int_get bind_common_onoff_inverted_get
#define bind_common_int_set bind_common_onoff_inverted_set

// BT_VideoDisplay: fullscreen display number
static OptionBinding* bind_video_display(int cfgentry) {
	OptionBinding *bind = bind_new();

	bind->configentry = cfgentry;
	bind->type = BT_VideoDisplay;

	bind->getter = bind_common_int_get;
	bind->setter = bind_common_int_set;

	bind->valrange_min = 0;
	bind->valrange_max = 0; // updated later

	bind->selected = video_current_display();

	return bind;
}

static int bind_common_intplus1_get(OptionBinding *b) {
	return config_get_int(b->configentry) - 1;
}

static int bind_common_intplus1_set(OptionBinding *b, int v) {
	return config_set_int(b->configentry, v + 1) - 1;
}


// --- Binding callbacks for individual options --- //

static bool bind_resizable_dependence(void) {
	return video_query_capability(VIDEO_CAP_EXTERNAL_RESIZE) == VIDEO_AVAILABLE;
}

static bool bind_bgquality_dependence(void) {
	return !config_get_int(CONFIG_NO_STAGEBG);
}

static bool bind_resolution_dependence(void) {
	return video_query_capability(VIDEO_CAP_CHANGE_RESOLUTION) == VIDEO_AVAILABLE;
}

static bool bind_fb_resolution_dependence(void) {
	return false;
}

static bool bind_fullscreen_dependence(void) {
	return video_query_capability(VIDEO_CAP_FULLSCREEN) == VIDEO_AVAILABLE;
}

static int bind_resolution_set(OptionBinding *b, int v) {
	bool fullscreen = video_is_fullscreen();
	if(v >= 0) {
		VideoMode m = video_get_mode(v, fullscreen);
		config_set_int(CONFIG_VID_WIDTH, m.width);
		config_set_int(CONFIG_VID_HEIGHT, m.height);
	}

	return v;
}

static int bind_power_set(OptionBinding *b, int v) {
	return config_set_int(b->configentry, v * 100) / 100;
}

static int bind_power_get(OptionBinding *b) {
	return config_get_int(b->configentry) / 100;
}

// --- Creating, destroying, filling the menu --- //

typedef struct OptionsMenuContext {
	const char *title;
	void *data;
} OptionsMenuContext;

static void destroy_options_menu(MenuData *m) {
	bool change_vidmode = false;

	dynarray_foreach_idx(&m->entries, int i, {
		OptionBinding *bind = bind_get(m, i);

		if(!bind) {
			continue;
		}

		if(bind->type == BT_Resolution && video_query_capability(VIDEO_CAP_CHANGE_RESOLUTION) == VIDEO_AVAILABLE) {
			if(bind->selected != -1) {
				bool fullscreen = video_is_fullscreen();
				VideoMode mode = video_get_mode(bind->selected, fullscreen);
				config_set_int(CONFIG_VID_WIDTH, mode.width);
				config_set_int(CONFIG_VID_HEIGHT, mode.height);
				change_vidmode = true;
			}
		}

		bind_free(bind);
		mem_free(bind);
	});

	if(change_vidmode) {
		video_set_mode(
			config_get_int(CONFIG_VID_DISPLAY),
			config_get_int(CONFIG_VID_WIDTH),
			config_get_int(CONFIG_VID_HEIGHT),
			config_get_int(CONFIG_FULLSCREEN),
			config_get_int(CONFIG_VID_RESIZABLE)
		);
	}

	if(m->context) {
		OptionsMenuContext *ctx = m->context;
		mem_free(ctx->data);
		mem_free(ctx);
	}
}

static void do_nothing(MenuData *menu, void *arg) { }
static void update_options_menu(MenuData *menu);
static void options_menu_input(MenuData*);
static void draw_options_menu(MenuData*);

#define bind_onoff(b) bind_addvalue(b, "on"); bind_addvalue(b, "off")

static bool entry_is_active(MenuData *m, int idx) {
	MenuEntry *e = dynarray_get_ptr(&m->entries, idx);

	if(!e->action) {
		return false;
	}

	OptionBinding *bind = bind_get(m, idx);

	if(bind) {
		return bind_isactive(bind);
	}

	return true;
}

static void begin_options_menu(MenuData *m) {
	dynarray_foreach_idx(&m->entries, int i, {
		if(entry_is_active(m, i)) {
			m->cursor = i;
			break;
		}
	});
}

static MenuData* create_options_menu_base(const char *s) {
	MenuData *m = alloc_menu();
	m->transition = TransFadeBlack;
	m->flags = MF_Abortable;
	m->input = options_menu_input;
	m->draw = draw_options_menu;
	m->logic = update_options_menu;
	m->begin = begin_options_menu;
	m->end = destroy_options_menu;
	m->context = ALLOC(OptionsMenuContext, { .title = s });

	return m;
}

static void options_enter_sub(MenuData *parent, MenuData *(*construct)(MenuData*)) {
	parent->frames = 0;
	enter_menu(construct(parent), NO_CALLCHAIN);
}

#define DECLARE_ENTER_FUNC(enter, construct) \
	static void enter(MenuData *parent, void *arg) { \
		options_enter_sub(parent, construct); \
	}

static MenuData* create_options_menu_controls(MenuData *parent);
DECLARE_ENTER_FUNC(enter_options_menu_controls, create_options_menu_controls)

static MenuData* create_options_menu_gamepad(MenuData *parent);
DECLARE_ENTER_FUNC(enter_options_menu_gamepad, create_options_menu_gamepad)

static MenuData* create_options_menu_gamepad_controls(MenuData *parent);
DECLARE_ENTER_FUNC(enter_options_menu_gamepad_controls, create_options_menu_gamepad_controls)

static MenuData* create_options_menu_video(MenuData *parent);
DECLARE_ENTER_FUNC(enter_options_menu_video, create_options_menu_video)

static MenuData* create_options_menu_video(MenuData *parent) {
	MenuData *m = create_options_menu_base("Video Options");
	OptionBinding *b;

	add_menu_entry(m, "Fullscreen", do_nothing,
		b = bind_option(CONFIG_FULLSCREEN, bind_common_onoff_get, bind_common_onoff_set)
	);	bind_onoff(b);
		b->dependence = bind_fullscreen_dependence;

	add_menu_entry(m, "Display", do_nothing,
		b = bind_video_display(CONFIG_VID_DISPLAY)
	);

	add_menu_entry(m, "Window size", do_nothing,
		b = bind_resolution()
	);	b->setter = bind_resolution_set;
		b->dependence = bind_resolution_dependence;

	add_menu_entry(m, "Renderer resolution", do_nothing,
		b = bind_fb_resolution()
	);	b->dependence = bind_fb_resolution_dependence;
		b->pad++;

	add_menu_entry(m, "Resizable window", do_nothing,
		b = bind_option(CONFIG_VID_RESIZABLE, bind_common_onoff_get, bind_common_onoff_set)
	);	bind_onoff(b);
		b->dependence = bind_resizable_dependence;

	add_menu_separator(m);

	add_menu_entry(m, "Pause the game when not focused", do_nothing,
		b = bind_option(CONFIG_FOCUS_LOSS_PAUSE, bind_common_onoff_get, bind_common_onoff_set)
	);	bind_onoff(b);

	add_menu_entry(m, "Vertical synchronization", do_nothing,
		b = bind_option(CONFIG_VSYNC, bind_common_onoffplus_get, bind_common_onoffplus_set)
	);	bind_addvalue(b, "on");
		bind_addvalue(b, "off");

	if(video_query_capability(VIDEO_CAP_VSYNC_ADAPTIVE) != VIDEO_NEVER_AVAILABLE) {
		bind_addvalue(b, "adaptive");
	}

	add_menu_entry(m, "Skip frames", do_nothing,
		b = bind_option(CONFIG_VID_FRAMESKIP, bind_common_intplus1_get, bind_common_intplus1_set)
	);	bind_addvalue(b, "0");
		bind_addvalue(b, "½");
		bind_addvalue(b, "⅔");

	add_menu_separator(m);

	add_menu_entry(m, "Overall rendering quality", do_nothing,
		b = bind_scale(CONFIG_FG_QUALITY, 0.1, 1.0, 0.05)
	);

	add_menu_entry(m, "Draw background", do_nothing,
		b = bind_option(CONFIG_NO_STAGEBG, bind_common_onoff_inverted_get, bind_common_onoff_inverted_set)
	);	bind_onoff(b);

	add_menu_entry(m, "Background rendering quality", do_nothing,
		b = bind_scale(CONFIG_BG_QUALITY, 0.1, 1.0, 0.05)
	);	b->dependence = bind_bgquality_dependence;
		b->pad++;

	add_menu_separator(m);

	add_menu_entry(m, "Anti-aliasing", do_nothing,
		b = bind_option(CONFIG_FXAA, bind_common_int_get, bind_common_int_set)
	);	bind_addvalue(b, "none");
		bind_addvalue(b, "fxaa");

	add_menu_entry(m, "Particle effects", do_nothing,
		b = bind_option(CONFIG_PARTICLES, bind_common_int_get, bind_common_int_set)
	);	bind_addvalue(b, "minimal");
		bind_addvalue(b, "full");

	add_menu_entry(m, "Postprocessing effects", do_nothing,
		b = bind_option(CONFIG_POSTPROCESS, bind_common_int_get, bind_common_int_set)
	);	bind_addvalue(b, "minimal");
		bind_addvalue(b, "fast");
		bind_addvalue(b, "full");

	add_menu_separator(m);
	add_menu_entry(m, "Back", menu_action_close, NULL);

	return m;
}

attr_unused
static void bind_setvaluerange_fancy(OptionBinding *b, int ma) {
	int i = 0; for(i = 0; i <= ma; ++i) {
		char tmp[16];
		snprintf(tmp, 16, "%i", i);
		bind_addvalue(b, tmp);
	}
}

#ifndef __SWITCH__
static bool gamepad_enabled_depencence(void) {
	return config_get_int(CONFIG_GAMEPAD_ENABLED);
}
#endif

static MenuData* create_options_menu_gamepad_controls(MenuData *parent) {
	MenuData *m = create_options_menu_base("Gamepad Controls");

	add_menu_entry(m, "Move up", do_nothing,
		bind_gpbinding(CONFIG_GAMEPAD_KEY_UP)
	);

	add_menu_entry(m, "Move down", do_nothing,
		bind_gpbinding(CONFIG_GAMEPAD_KEY_DOWN)
	);

	add_menu_entry(m, "Move left", do_nothing,
		bind_gpbinding(CONFIG_GAMEPAD_KEY_LEFT)
	);

	add_menu_entry(m, "Move right", do_nothing,
		bind_gpbinding(CONFIG_GAMEPAD_KEY_RIGHT)
	);

	add_menu_separator(m);

	add_menu_entry(m, "Shoot", do_nothing,
		bind_gpbinding(CONFIG_GAMEPAD_KEY_SHOT)
	);

	add_menu_entry(m, "Focus", do_nothing,
		bind_gpbinding(CONFIG_GAMEPAD_KEY_FOCUS)
	);

	add_menu_entry(m, "Use Spell Card", do_nothing,
		bind_gpbinding(CONFIG_GAMEPAD_KEY_BOMB)
	);

	add_menu_entry(m, "Power Surge / Discharge", do_nothing,
		bind_gpbinding(CONFIG_GAMEPAD_KEY_SPECIAL)
	);

	add_menu_separator(m);

	add_menu_entry(m, "Skip dialog", do_nothing,
		bind_gpbinding(CONFIG_GAMEPAD_KEY_SKIP)
	);

	add_menu_separator(m);
	add_menu_entry(m, "Back", menu_action_close, NULL);

	return m;
}

static void destroy_options_menu_gamepad(MenuData *m) {
	OptionsMenuContext *ctx = m->context;

	if(config_get_int(CONFIG_GAMEPAD_ENABLED) && strcasecmp(config_get_str(CONFIG_GAMEPAD_DEVICE), ctx->data)) {
		gamepad_update_devices();
	}

	destroy_options_menu(m);
}

static MenuData* create_options_menu_gamepad(MenuData *parent) {
	MenuData *m = create_options_menu_base("Gamepad Options");
	m->end = destroy_options_menu_gamepad;

	OptionsMenuContext *ctx = m->context;
	ctx->data = strdup(config_get_str(CONFIG_GAMEPAD_DEVICE));

	OptionBinding *b;

#ifndef __SWITCH__
	add_menu_entry(m, "Enable Gamepad/Joystick support", do_nothing,
		b = bind_option(CONFIG_GAMEPAD_ENABLED, bind_common_onoff_get, bind_common_onoff_set)
	);	bind_onoff(b);

	add_menu_entry(m, "Device", do_nothing,
		b = bind_gpdevice(CONFIG_GAMEPAD_DEVICE)
	);	b->dependence = gamepad_enabled_depencence;
#endif

	add_menu_separator(m);
	add_menu_entry(m, "Customize controls…", enter_options_menu_gamepad_controls, NULL);

	add_menu_separator(m);

	add_menu_entry(m, "X axis", do_nothing,
		b = bind_gpaxisbinding(CONFIG_GAMEPAD_AXIS_LR)
	);

	add_menu_entry(m, "Y axis", do_nothing,
		b = bind_gpaxisbinding(CONFIG_GAMEPAD_AXIS_UD)
	);

	add_menu_entry(m, "Axes mode", do_nothing,
		b = bind_option(CONFIG_GAMEPAD_AXIS_FREE, bind_common_onoff_get, bind_common_onoff_set)
	);	bind_addvalue(b, "free");
		bind_addvalue(b, "restricted");

	add_menu_entry(m, "X axis sensitivity", do_nothing,
		b = bind_scale(CONFIG_GAMEPAD_AXIS_LR_SENS, -2, 2, 0.05)
	);	b->pad++;

	add_menu_entry(m, "Y axis sensitivity", do_nothing,
		b = bind_scale(CONFIG_GAMEPAD_AXIS_UD_SENS, -2, 2, 0.05)
	);	b->pad++;

	add_menu_entry(m, "Dead zone", do_nothing,
		b = bind_scale(CONFIG_GAMEPAD_AXIS_DEADZONE, 0, 1, 0.01)
	);

	add_menu_separator(m);
	add_menu_entry(m, "Back", menu_action_close, NULL);

	return m;
}

static MenuData* create_options_menu_controls(MenuData *parent) {
	MenuData *m = create_options_menu_base("Controls");

	add_menu_entry(m, "Move up", do_nothing,
		bind_keybinding(CONFIG_KEY_UP)
	);

	add_menu_entry(m, "Move down", do_nothing,
		bind_keybinding(CONFIG_KEY_DOWN)
	);

	add_menu_entry(m, "Move left", do_nothing,
		bind_keybinding(CONFIG_KEY_LEFT)
	);

	add_menu_entry(m, "Move right", do_nothing,
		bind_keybinding(CONFIG_KEY_RIGHT)
	);

	add_menu_separator(m);

	add_menu_entry(m, "Shoot", do_nothing,
		bind_keybinding(CONFIG_KEY_SHOT)
	);

	add_menu_entry(m, "Focus", do_nothing,
		bind_keybinding(CONFIG_KEY_FOCUS)
	);

	add_menu_entry(m, "Use Spell Card", do_nothing,
		bind_keybinding(CONFIG_KEY_BOMB)
	);

	add_menu_entry(m, "Power Surge / Discharge", do_nothing,
		bind_keybinding(CONFIG_KEY_SPECIAL)
	);

	add_menu_separator(m);

	add_menu_entry(m, "Toggle fullscreen", do_nothing,
		bind_keybinding(CONFIG_KEY_FULLSCREEN)
	);

	add_menu_entry(m, "Take a screenshot", do_nothing,
		bind_keybinding(CONFIG_KEY_SCREENSHOT)
	);

	add_menu_entry(m, "Skip dialog", do_nothing,
		bind_keybinding(CONFIG_KEY_SKIP)
	);

	add_menu_separator(m);

	add_menu_entry(m, "Toggle audio", do_nothing,
		bind_keybinding(CONFIG_KEY_TOGGLE_AUDIO)
	);

	add_menu_separator(m);

	add_menu_entry(m, "Stop the game immediately", do_nothing,
		bind_keybinding(CONFIG_KEY_STOP)
	);

	add_menu_entry(m, "Restart the game immediately", do_nothing,
		bind_keybinding(CONFIG_KEY_RESTART)
	);

	add_menu_entry(m, "Quick save", do_nothing,
		bind_keybinding(CONFIG_KEY_QUICKSAVE)
	);

	add_menu_entry(m, "Quick load", do_nothing,
		bind_keybinding(CONFIG_KEY_QUICKLOAD)
	);

#ifdef DEBUG
	add_menu_separator(m);

	add_menu_entry(m, "Toggle God mode", do_nothing,
		bind_keybinding(CONFIG_KEY_IDDQD)
	);

	add_menu_entry(m, "Skip stage", do_nothing,
		bind_keybinding(CONFIG_KEY_HAHAIWIN)
	);

	add_menu_entry(m, "Power up", do_nothing,
		bind_keybinding(CONFIG_KEY_POWERUP)
	);

	add_menu_entry(m, "Power down", do_nothing,
		bind_keybinding(CONFIG_KEY_POWERDOWN)
	);

	add_menu_entry(m, "Disable background rendering (HoM effect)", do_nothing,
		bind_keybinding(CONFIG_KEY_NOBACKGROUND)
	);

	add_menu_entry(m, "Toggle collision areas overlay", do_nothing,
		bind_keybinding(CONFIG_KEY_HITAREAS)
	);
#endif

	add_menu_separator(m);
	add_menu_entry(m, "Back", menu_action_close, NULL);

	return m;
}

MenuData* create_options_menu(void) {
	MenuData *m = create_options_menu_base("Options");
	OptionBinding *b;

#ifndef __SWITCH__
	add_menu_entry(m, "Player name", do_nothing,
		b = bind_stroption(CONFIG_PLAYERNAME)
	);

	add_menu_separator(m);
#endif

	add_menu_entry(m, "Save replays", do_nothing,
		b = bind_option(CONFIG_SAVE_RPY, bind_common_onoffplus_get, bind_common_onoffplus_set)
	);	bind_addvalue(b, "always");
		bind_addvalue(b, "never");
		bind_addvalue(b, "ask");

	add_menu_entry(m, "Auto-restart in Spell Practice", do_nothing,
		b = bind_option(CONFIG_SPELLSTAGE_AUTORESTART,  bind_common_onoff_get, bind_common_onoff_set)
	);	bind_onoff(b);

	add_menu_entry(m, "Power in Spell and Stage Practice", do_nothing,
		b = bind_option(CONFIG_PRACTICE_POWER, bind_power_get, bind_power_set)
	);	bind_addvalue(b, "0.0");
		bind_addvalue(b, "1.0");
		bind_addvalue(b, "2.0");
		bind_addvalue(b, "3.0");
		bind_addvalue(b, "4.0");

	add_menu_entry(m, "Shoot by default", do_nothing,
		b = bind_option(CONFIG_SHOT_INVERTED,   bind_common_onoff_get, bind_common_onoff_set)
	);	bind_onoff(b);

	add_menu_entry(m, "Boss healthbar style", do_nothing,
		b = bind_option(CONFIG_HEALTHBAR_STYLE,   bind_common_int_get, bind_common_int_set)
	);	bind_addvalue(b, "classic");
		bind_addvalue(b, "modern");

	add_menu_entry(m, "Floating score text visibility", do_nothing,
		b = bind_scale(CONFIG_SCORETEXT_ALPHA, 0, 1, 0.05)
	);

	add_menu_separator(m);

	add_menu_entry(m, "SFX Volume", do_nothing,
		b = bind_scale(CONFIG_SFX_VOLUME, 0, 1, 0.05)
	);	b->dependence = audio_output_works;

	add_menu_entry(m, "BGM Volume", do_nothing,
		b = bind_scale(CONFIG_BGM_VOLUME, 0, 1, 0.05)
	);	b->dependence = audio_output_works;

	add_menu_entry(m, "Mute audio", do_nothing,
		b = bind_option(CONFIG_MUTE_AUDIO,  bind_common_onoff_get, bind_common_onoff_set)
	);	bind_onoff(b);

	add_menu_separator(m);
	add_menu_entry(m, "Video options…", enter_options_menu_video, NULL);
	add_menu_entry(m, "Customize controls…", enter_options_menu_controls, NULL);
	add_menu_entry(m, "Gamepad & Joystick options…", enter_options_menu_gamepad, NULL);
	add_menu_separator(m);

	add_menu_entry(m, "Back", menu_action_close, NULL);

	return m;
}

// --- Drawing the menu --- //

void draw_options_menu_bg(MenuData* menu) {
	draw_main_menu_bg(menu, 0, 0, 0.05, "abstract_brown", "stage1/cirnobg");

	r_state_push();
	r_mat_mv_push();
	r_mat_mv_scale(SCREEN_W, SCREEN_H, 1);
	r_shader_standard_notex();
	r_mat_mv_translate(0.5, 0.5, 0);
	r_color(RGBA(0, 0, 0, 0.5));
	r_draw_quad();
	r_mat_mv_pop();
	r_state_pop();
}

static void update_options_menu(MenuData *menu) {
	menu->drawdata[0] += ((SCREEN_W/2 - 100) - menu->drawdata[0])/10.0;
	menu->drawdata[1] += ((SCREEN_W - 200) * 1.75 - menu->drawdata[1])/10.0;
	menu->drawdata[2] += (20*menu->cursor - menu->drawdata[2])/10.0;

	animate_menu_list_entries(menu);
}

static void options_draw_item(MenuEntry *e, int i, int cnt, void *ctx) {
	MenuData *menu = ctx;
	OptionBinding *bind = bind_get(menu, i);
	Color clr;

	if(!e->name) {
		return;
	}

	r_shader("text_default");

	float a = e->drawdata * 0.1;
	float alpha = entry_is_active(menu, i) ? 1 : 0.5;

	clr = *r_color_current();

	if(!entry_is_active(menu, i)) {
		color_mul_scalar(&clr, 0.5f);
	}

	text_draw(e->name, &(TextParams) {
		.pos = { (1 + (bind ? bind->pad : 0)) * 20 - e->drawdata, 20*i },
		.color = &clr,
	});

	if(bind) {
		int j, origin = SCREEN_W - 220;

		switch(bind->type) {
			case BT_IntValue: {
				int val = bind_getvalue(bind);

				if(bind->configentry == CONFIG_PRACTICE_POWER) {
					Font *fnt_int = res_font("standard");
					Font *fnt_fract = res_font("small");

					draw_fraction(
						val,
						ALIGN_RIGHT,
						origin,
						20*i,
						fnt_int,
						fnt_fract,
						&clr,
						&clr,
						false
					);
				} else if(bind->values) {
					for(j = bind->displaysingle? val : bind->valrange_max; (j+1) && (!bind->displaysingle || j == val); --j) {
						if(j != bind->valrange_max && !bind->displaysingle) {
							origin -= text_width(res_font("standard"), bind->values[j+1], 0) + 5;
						}

						if(val == j) {
							clr = *RGBA_MUL_ALPHA(0.9, 0.6, 0.2, alpha);
						} else {
							clr = *RGBA_MUL_ALPHA(0.5, 0.5, 0.5, 0.7 * alpha);
						}

						text_draw(bind->values[j], &(TextParams) {
							.pos = { origin, 20*i },
							.align = ALIGN_RIGHT,
							.color = &clr,
						});
					}
				} else {
					char tmp[16];   // who'd use a 16-digit number here anyway?
					snprintf(tmp, 16, "%d", bind_getvalue(bind));

					text_draw(tmp, &(TextParams) {
						.pos = { origin, 20*i },
						.align = ALIGN_RIGHT,
						.color = &clr,
					});
				}
				break;
			}

			case BT_KeyBinding: {
				if(bind->blockinput) {
					text_draw("Press a key to assign, ESC to cancel", &(TextParams) {
						.pos = { origin, 20*i },
						.align = ALIGN_RIGHT,
						.color = RGBA(0.5, 1, 0.5, 1),
					});
				} else {
					const char *txt = SDL_GetScancodeName(config_get_int(bind->configentry));

					if(!txt || !*txt) {
						txt = "Unknown";
					}

					text_draw(txt, &(TextParams) {
						.pos = { origin, 20*i },
						.align = ALIGN_RIGHT,
						.color = &clr,
					});
				}

				break;
			}

			case BT_GamepadDevice: {
				if(bind_isactive(bind)) {
					// XXX: I'm not exactly a huge fan of fixing up state in drawing code, but it seems the way to go for now...
					bind->valrange_max = gamepad_device_count() - 1;

					if(bind->selected < -1 || bind->selected > bind->valrange_max) {
						bind->selected = gamepad_get_active_device();

						if(bind->selected < -1) {
							bind->selected = -1;
						}
					}

					char *txt;
					char buf[64];

					if(bind->valrange_max >= 0) {
						if(bind->selected < 0) {
							txt = "All devices";
						} else {
							snprintf(buf, sizeof(buf), "#%i: %s", bind->selected + 1, gamepad_device_name(bind->selected));
							txt = buf;
						}
					} else {
						txt = "No devices available";
					}

					text_draw(txt, &(TextParams) {
						.pos = { origin, 20*i },
						.align = ALIGN_RIGHT,
						.color = &clr,
						.max_width = (SCREEN_W - 220) / 2,
					});
				}

				break;
			}

			case BT_VideoDisplay: {
				// XXX: see the BT_GamepadDevice case...
				bind->valrange_max = video_num_displays() - 1;

				if(bind->selected < 0 || bind->selected > bind->valrange_max) {
					bind->selected = video_current_display();

					if(bind->selected < 0) {
						bind->selected = 0;
					}
				}

				char buf[64];
				snprintf(buf, sizeof(buf), "#%i: %s", bind->selected + 1, video_display_name(bind->selected));
				text_draw(buf, &(TextParams) {
					.pos = { origin, 20*i },
					.align = ALIGN_RIGHT,
					.color = &clr,
					.max_width = (SCREEN_W - 220) / 2,
				});
				break;
			}

			case BT_GamepadKeyBinding:
			case BT_GamepadAxisBinding: {
				bool is_axis = (bind->type == BT_GamepadAxisBinding);

				if(bind->blockinput) {
					char *text = is_axis ? "Move an axis to assign, Back to cancel"
											: "Press a button to assign, Back to cancel";

					text_draw(text, &(TextParams) {
						.pos = { origin, 20*i },
						.align = ALIGN_RIGHT,
						.color = RGBA(0.5, 1, 0.5, 1),
					});
				} else if(config_get_int(bind->configentry) >= 0) {
					int id = config_get_int(bind->configentry);
					const char *name = (is_axis ? gamepad_axis_name(id) : gamepad_button_name(id));
					text_draw(name, &(TextParams) {
						.pos = { origin, 20*i },
						.align = ALIGN_RIGHT,
						.color = &clr,
					});
				} else {
					text_draw("Unbound", &(TextParams) {
						.pos = { origin, 20*i },
						.align = ALIGN_RIGHT,
						.color = &clr,
					});
				}
				break;
			}

			case BT_StrValue: {
				if(bind->blockinput) {
					if(*bind->strvalue) {
						text_draw(bind->strvalue, &(TextParams) {
							.pos = { origin, 20*i },
							.align = ALIGN_RIGHT,
							.color = RGBA(0.5, 1, 0.5, 1.0),
						});
					}
				} else {
					text_draw(config_get_str(bind->configentry), &(TextParams) {
						.pos = { origin, 20*i },
						.align = ALIGN_RIGHT,
						.color = &clr,
					});
				}
				break;
			}

			case BT_Resolution: {
				char tmp[32];
				int w, h;
				VideoMode m;

				if(bind->selected == -1) {
					m = video_get_current_mode();
				} else {
					bool fullscreen = video_is_fullscreen();
					m = video_get_mode(bind->selected, fullscreen);
				}

				w = m.width;
				h = m.height;

				snprintf(tmp, sizeof(tmp), "%dx%d", w, h);
				text_draw(tmp, &(TextParams) {
					.pos = { origin, 20*i },
					.align = ALIGN_RIGHT,
					.color = &clr,
				});
				break;
			}

			case BT_FramebufferResolution: {
				IntExtent fbsize = r_framebuffer_get_size(video_get_screen_framebuffer());
				char tmp[32];
				snprintf(tmp, sizeof(tmp), "%gx  %dx%d", video_get_scaling_factor(), fbsize.w, fbsize.h);
				text_draw(tmp, &(TextParams) {
					.pos = { origin, 20*i },
					.align = ALIGN_RIGHT,
					.color = &clr,
				});
				break;
			}

			case BT_Scale: {
				int w  = 200;
				int h  = 5;
				int cw = 5;

				float val = config_get_float(bind->configentry);

				float ma = bind->scale_max;
				float mi = bind->scale_min;
				float pos = (val - mi) / (ma - mi);

				char tmp[8];
				snprintf(tmp, 8, "%.0f%%", 100 * val);
				if(!strcmp(tmp, "-0%"))
					strcpy(tmp, "0%");

				r_mat_mv_push();
				r_mat_mv_translate(origin - (w+cw) * 0.5, 20 * i, 0);
				text_draw(tmp, &(TextParams) {
					.pos = { -((w+cw) * 0.5 + 10), 0 },
					.align = ALIGN_RIGHT,
					.color = &clr,
				});
				r_shader_standard_notex();
				r_mat_mv_push();
				r_mat_mv_scale(w+cw, h, 1);
				r_color(RGBA_MUL_ALPHA(1, 1, 1, (0.1 + 0.2 * a) * alpha));
				r_draw_quad();
				r_mat_mv_pop();
				r_mat_mv_translate(w * (pos - 0.5), 0, 0);
				r_mat_mv_scale(cw, h, 0);
				r_color(RGBA_MUL_ALPHA(0.9, 0.6, 0.2, alpha));
				r_draw_quad();
				r_mat_mv_pop();
				r_shader("text_default");

				break;
			}
		}
	}
}

static void draw_options_menu(MenuData *menu) {
	OptionsMenuContext *ctx = menu->context;
	r_state_push();
	draw_options_menu_bg(menu);
	draw_menu_title(menu, ctx->title);
	draw_menu_list(menu, 100, 100, options_draw_item, SCREEN_H * 1.1, menu);
	r_state_pop();
}

// --- Input/event processing --- //

static bool options_vidmode_change_handler(SDL_Event *event, void *arg) {
	MenuData *menu = arg;

	dynarray_foreach_idx(&menu->entries, int i, {
		OptionBinding *bind = bind_get(menu, i);

		if(!bind) {
			continue;
		}

		switch(bind->type) {
			case BT_Resolution:
				bind_resolution_update(bind);
				break;

			case BT_IntValue:
				if(bind->configentry == CONFIG_FULLSCREEN) {
					bind->selected = video_is_fullscreen();
				}
				break;

			default:
				break;
		}
	});

	return false;
}

static bool options_rebind_input_handler(SDL_Event *event, void *arg) {
	if(!arg) {
		return false;
	}

	OptionBinding *b = arg;
	uint32_t t = event->type;

	if(b->type == BT_StrValue) {
		return false;
	}

	if(t == SDL_KEYDOWN) {
		SDL_Scancode scan = event->key.keysym.scancode;
		bool esc = scan == SDL_SCANCODE_ESCAPE;

		if(b->type != BT_KeyBinding) {
			if(esc) {
				b->blockinput = false;
				play_sfx_ui("hit");
			}

			return true;
		}

		if(!esc) {
			for(int i = CONFIG_KEY_FIRST; i <= CONFIG_KEY_LAST; ++i) {
				if(config_get_int(i) == scan) {
					config_set_int(i, config_get_int(b->configentry));
				}
			}

			config_set_int(b->configentry, scan);
			play_sfx_ui("shot_special1");
		} else {
			play_sfx_ui("hit");
		}

		b->blockinput = false;
		return true;
	}

	if(t == MAKE_TAISEI_EVENT(TE_MENU_ABORT)) {
		play_sfx_ui("hit");
		b->blockinput = false;
		return true;
	}

	if(t == MAKE_TAISEI_EVENT(TE_GAMEPAD_BUTTON_DOWN)) {
		GamepadButton button = event->user.code;

		if(b->type != BT_GamepadKeyBinding) {
			/*
			if(b->type == BT_GamepadAxisBinding) {
				b->blockinput = false;
				play_ui_sound("hit");
			}
			*/

			return true;
		}

		if(button == GAMEPAD_BUTTON_BACK || button == GAMEPAD_BUTTON_START) {
			b->blockinput = false;
			play_sfx_ui("hit");
			return true;
		}

		for(int i = CONFIG_GAMEPAD_KEY_FIRST; i <= CONFIG_GAMEPAD_KEY_LAST; ++i) {
			if(config_get_int(i) == button) {
				config_set_int(i, config_get_int(b->configentry));
			}
		}

		config_set_int(b->configentry, button);
		b->blockinput = false;
		play_sfx_ui("shot_special1");
		return true;
	}

	if(t == MAKE_TAISEI_EVENT(TE_GAMEPAD_AXIS)) {
		GamepadAxis axis = event->user.code;

		if(b->type == BT_GamepadAxisBinding) {
			if(b->configentry == CONFIG_GAMEPAD_AXIS_UD) {
				if(config_get_int(CONFIG_GAMEPAD_AXIS_LR) == axis) {
					config_set_int(CONFIG_GAMEPAD_AXIS_LR, config_get_int(CONFIG_GAMEPAD_AXIS_UD));
				}
			} else if(b->configentry == CONFIG_GAMEPAD_AXIS_LR) {
				if(config_get_int(CONFIG_GAMEPAD_AXIS_UD) == axis) {
					config_set_int(CONFIG_GAMEPAD_AXIS_UD, config_get_int(CONFIG_GAMEPAD_AXIS_LR));
				}
			}

			config_set_int(b->configentry, axis);
			b->blockinput = false;
		}

		return true;
	}

	return true;
}

static bool options_text_input_handler(SDL_Event *event, void *arg) {
	OptionBinding *b = arg;

	if(!b || b->type != BT_StrValue) {
		return false;
	}

	uint32_t t = event->type;

	if(t == SDL_TEXTINPUT || t == MAKE_TAISEI_EVENT(TE_CLIPBOARD_PASTE)) {
		const size_t max_len = 32;
		const char *snd = "generic_shot";
		char *text, *text_allocated = NULL;

		if(t == SDL_TEXTINPUT) {
			text = event->text.text;
		} else {
			text = text_allocated = SDL_GetClipboardText();
		}

		assert(text != NULL);
		strappend(&b->strvalue, text);

		if(strlen(b->strvalue) > max_len) {
			/*
			 * EFFICIENT AS FUCK
			 */

			uint32_t *u = utf8_to_ucs4_alloc(b->strvalue);
			size_t ulen = ucs4len(u);

			if(ulen > max_len) {
				*(u + max_len) = 0;
				mem_free(b->strvalue);
				b->strvalue = ucs4_to_utf8_alloc(u);
				snd = "hit";
			}

			mem_free(u);
		}

		mem_free(text_allocated);
		play_sfx_ui(snd);
		return true;
	}

	if(t == SDL_KEYDOWN) {
		SDL_Scancode scan = event->key.keysym.scancode;

		if(scan == SDL_SCANCODE_ESCAPE) {
			play_sfx_ui("hit");
			stralloc(&b->strvalue, config_get_str(b->configentry));
			b->blockinput = false;
		} else if(scan == SDL_SCANCODE_RETURN) {
			if(*b->strvalue) {
				play_sfx_ui("shot_special1");
			} else {
				play_sfx_ui("hit");
				stralloc(&b->strvalue, "Player");
			}

			config_set_str(b->configentry, b->strvalue);
			b->blockinput = false;
		} else if(scan == SDL_SCANCODE_BACKSPACE) {
			/*
			 * MORE EFFICIENT THAN FUCK
			 */

			uint32_t *u = utf8_to_ucs4_alloc(b->strvalue);

			if(*u) {
				play_sfx_ui("generic_shot");
				*(ucs4chr(u, 0) - 1) = 0;
			} else {
				play_sfx_ui("hit");
			}

			mem_free(b->strvalue);
			b->strvalue = ucs4_to_utf8_alloc(u);
			mem_free(u);
		}

		return true;
	}

	return true;
}

static bool options_input_handler(SDL_Event *event, void *arg) {
	MenuData *menu = arg;
	OptionBinding *bind = bind_get(menu, menu->cursor);
	TaiseiEvent type = TAISEI_EVENT(event->type);

	switch(type) {
		case TE_MENU_CURSOR_UP:
		case TE_MENU_CURSOR_DOWN:
			play_sfx_ui("generic_shot");
			menu->drawdata[3] = 10;
			do {
				menu->cursor += (type == TE_MENU_CURSOR_UP ? -1 : 1);

				if(menu->cursor >= menu->entries.num_elements) {
					menu->cursor = 0;
				}

				if(menu->cursor < 0) {
					menu->cursor = menu->entries.num_elements - 1;
				}

			} while(!entry_is_active(menu, menu->cursor));
		break;

		case TE_MENU_CURSOR_LEFT:
		case TE_MENU_CURSOR_RIGHT:
			play_sfx_ui("generic_shot");
			bool next = (type == TE_MENU_CURSOR_RIGHT);

			if(bind) {
				switch(bind->type) {
					case BT_GamepadDevice:
					case BT_IntValue:
					case BT_Resolution:
					case BT_VideoDisplay:
						(next ? bind_setnext : bind_setprev)(bind);
						break;

					case BT_Scale:
						config_set_float(bind->configentry,
							clamp(
								config_get_float(bind->configentry) + bind->scale_step * (next ? 1 : -1),
								bind->scale_min, bind->scale_max
							)
						);
						break;

					default:
						break;
				}
			}
		break;

		case TE_MENU_ACCEPT:
			play_sfx_ui("shot_special1");
			menu->selected = menu->cursor;

			if(bind) switch(bind->type) {
				case BT_KeyBinding:
				case BT_GamepadKeyBinding:
				case BT_GamepadAxisBinding:
					bind->blockinput = true;
					break;

				case BT_StrValue:
					bind->blockinput = true;
					break;

				default:
					break;
			} else close_menu(menu);
		break;

		case TE_MENU_ABORT:
			play_sfx_ui("hit");
			menu->selected = -1;
			close_menu(menu);
		break;

		default: break;
	}

	menu->cursor = (menu->cursor % menu->entries.num_elements) + menu->entries.num_elements * (menu->cursor < 0);
	return false;
}
#undef SHOULD_SKIP

static void options_menu_input(MenuData *menu) {
	OptionBinding *b;
	EventFlags flags = EFLAG_MENU;

	if((b = bind_getinputblocking(menu)) != NULL) {
		if(b->type == BT_StrValue) {
			flags |= EFLAG_TEXT;
		}
	}

	events_poll((EventHandler[]){
		{
			.proc = options_vidmode_change_handler,
			.arg = menu,
			.priority = EPRIO_SYSTEM,
			.event_type = MAKE_TAISEI_EVENT(TE_VIDEO_MODE_CHANGED),
		},
		{
			.proc = options_text_input_handler,
			.arg = b,
			.priority = EPRIO_CAPTURE,
		},
		{
			.proc = options_rebind_input_handler,
			.arg = b,
			.priority = EPRIO_CAPTURE,
		},
		{
			.proc = options_input_handler,
			.arg = menu,
			.priority = EPRIO_NORMAL,
		},

		{NULL}
	}, flags);
}
