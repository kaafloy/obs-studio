#include <windows.h>
#include <winuser.h>

#include <obs-module.h>
#include <util/dstr.h>

#include "cursor-capture.h"
#include "window-helpers.h"

#define do_log(level, format, ...)                                \
	blog(level, "[duplicator-monitor-capture: '%s'] " format, \
	     obs_source_get_name(capture->source), ##__VA_ARGS__)

#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG, format, ##__VA_ARGS__)

/* clang-format off */

#define TEXT_MONITOR_CAPTURE obs_module_text("MonitorCapture")
#define TEXT_CAPTURE_CURSOR  obs_module_text("CaptureCursor")
#define TEXT_CAPTURE_FOREGROUND_WINDOW obs_module_text("CaptureForegroundWindow")
#define TEXT_COMPATIBILITY   obs_module_text("Compatibility")
#define TEXT_MONITOR         obs_module_text("Monitor")
#define TEXT_PRIMARY_MONITOR obs_module_text("PrimaryMonitor")

/* clang-format on */

#define RESET_INTERVAL_SEC 3.0f

struct region {
	long x;
	long y;
	uint32_t width;
	uint32_t height;
} typedef region_t;

struct duplicator_capture {
	obs_source_t *source;
	int monitor;
	int rot;

	bool capture_cursor;
	bool capture_foreground_window;
	bool showing;

	region_t capture;
	region_t target;
	region_t display;

	gs_duplicator_t *duplicator;
	float reset_timeout;
	struct cursor_data cursor_data;
};

/* ------------------------------------------------------------------------- */

static inline void update_settings(struct duplicator_capture *capture,
				   obs_data_t *settings)
{
	capture->monitor = (int)obs_data_get_int(settings, "monitor");
	capture->capture_cursor = obs_data_get_bool(settings, "capture_cursor");
	capture->capture_foreground_window =
		obs_data_get_bool(settings, "capture_foreground_window");

	obs_enter_graphics();

	gs_duplicator_destroy(capture->duplicator);
	capture->duplicator = NULL;
	memset(&capture->capture, 0, sizeof(region_t));
	memset(&capture->target, 0, sizeof(region_t));
	memset(&capture->display, 0, sizeof(region_t));
	capture->rot = 0;
	capture->reset_timeout = RESET_INTERVAL_SEC;

	obs_leave_graphics();
}

/* ------------------------------------------------------------------------- */

static const char *duplicator_capture_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return TEXT_MONITOR_CAPTURE;
}

static void duplicator_capture_destroy(void *data)
{
	struct duplicator_capture *capture = data;

	obs_enter_graphics();

	gs_duplicator_destroy(capture->duplicator);
	cursor_data_free(&capture->cursor_data);

	obs_leave_graphics();

	bfree(capture);
}

static void duplicator_capture_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "monitor", 0);
	obs_data_set_default_bool(settings, "capture_cursor", true);
	obs_data_set_default_bool(settings, "capture_focusedwindow", false);
}

static void duplicator_capture_update(void *data, obs_data_t *settings)
{
	struct duplicator_capture *mc = data;
	update_settings(mc, settings);
}

static void *duplicator_capture_create(obs_data_t *settings,
				       obs_source_t *source)
{
	struct duplicator_capture *capture;

	capture = bzalloc(sizeof(struct duplicator_capture));
	capture->source = source;

	update_settings(capture, settings);

	return capture;
}

static void reset_capture_data(struct duplicator_capture *capture)
{
	struct gs_monitor_info monitor_info = {0};
	gs_texture_t *texture = gs_duplicator_get_texture(capture->duplicator);

	gs_get_duplicator_monitor_info(capture->monitor, &monitor_info);
	capture->display.width = capture->capture.width =
		gs_texture_get_width(texture);
	capture->display.height = capture->capture.height =
		gs_texture_get_height(texture);
	capture->display.x = capture->capture.x = monitor_info.x;
	capture->display.y = capture->capture.y = monitor_info.y;

	capture->rot = monitor_info.rotation_degrees;
}

static void free_capture_data(struct duplicator_capture *capture)
{
	gs_duplicator_destroy(capture->duplicator);
	cursor_data_free(&capture->cursor_data);
	capture->duplicator = NULL;
	memset(&capture->capture, 0, sizeof(region_t));
	memset(&capture->target, 0, sizeof(region_t));
	memset(&capture->display, 0, sizeof(region_t));
	capture->rot = 0;
	capture->reset_timeout = 0.0f;
}

static bool is_valid_window(struct duplicator_capture *capture, HWND hwnd)
{
	struct dstr name = {0};

	POINT monitor_topleft = {capture->display.x, capture->display.y};
	HMONITOR monitor_capture =
		MonitorFromPoint(monitor_topleft, MONITOR_DEFAULTTONEAREST);
	HMONITOR monitor_window =
		MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
	if (monitor_capture != monitor_window)
		return false;

	if (get_window_exe(&name, hwnd)) {
		if (dstr_find_i(&name, "explorer.exe")) {
			//return false;
		}

		//blog(LOG_INFO, "%s", name.array);
		dstr_free(&name);
	}

	LONG style = GetWindowLong(hwnd, GWL_STYLE);
	if (style &= WS_CAPTION)
		return true;

	return false;
}

static inline float lerp(float a, float b, float x)
{
	a *= 100;
	b *= 100;
	return ((1.0f - x) * a + x * b) / 100.0f;
}

static void duplicator_capture_tick(void *data, float seconds)
{
	struct duplicator_capture *capture = data;

	if (capture->capture_foreground_window) {
		HWND hwnd = GetForegroundWindow();
		while (true) {
			HWND parent = GetParent(hwnd);
			if (parent == NULL)
				break;
			hwnd = parent;
		}

		if (!is_valid_window(capture, hwnd)) {
			capture->target.x = capture->display.x;
			capture->target.y = capture->display.y;
			capture->target.width = capture->display.width;
			capture->target.height = capture->display.height;
		} else {
			RECT rect;
			GetClientRect(hwnd, &rect);

			POINT topLeft;
			topLeft.x = rect.left;
			topLeft.y = rect.top;

			POINT bottomRight;
			bottomRight.x = rect.right;
			bottomRight.y = rect.bottom;

			ClientToScreen(hwnd, &topLeft);
			ClientToScreen(hwnd, &bottomRight);

			long x = topLeft.x;
			long y = topLeft.y;
			uint32_t width = bottomRight.x - topLeft.x;
			uint32_t height = bottomRight.y - topLeft.y;

			if (capture->target.x != x || capture->target.y != y ||
			    capture->target.width != width ||
			    capture->target.height != height) {

				capture->target.x = x;
				capture->target.y = y;
				capture->target.width = width;
				capture->target.height = height;

				info("%i, %i (%i x %i)", capture->target.x,
				     capture->target.y, capture->target.width,
				     capture->target.height);
			}
		}

		if (capture->capture.x != capture->target.x ||
		    capture->capture.y != capture->target.y ||
		    capture->capture.width != capture->target.width ||
		    capture->capture.height != capture->target.height) {
			const float factor = seconds * 30.0f;
			capture->capture.x = (long)round(lerp(
				capture->target.x, capture->capture.x, factor));
			capture->capture.y = (long)round(lerp(
				capture->target.y, capture->capture.y, factor));
			capture->capture.width = (long)round(lerp(capture->target.width,
					   capture->capture.width, factor));
			capture->capture.height =(long)round(lerp(capture->target.height,
					   capture->capture.height, factor));
		}
	}

	/* completely shut down monitor capture if not in use, otherwise it can
	 * sometimes generate system lag when a game is in fullscreen mode */
	if (!obs_source_showing(capture->source)) {
		if (capture->showing) {
			obs_enter_graphics();
			free_capture_data(capture);
			obs_leave_graphics();

			capture->showing = false;
		}
		return;

		/* always try to load the capture immediately when the source is first
	 * shown */
	} else if (!capture->showing) {
		capture->reset_timeout = RESET_INTERVAL_SEC;
	}

	obs_enter_graphics();

	if (!capture->duplicator) {
		capture->reset_timeout += seconds;

		if (capture->reset_timeout >= RESET_INTERVAL_SEC) {
			capture->duplicator =
				gs_duplicator_create(capture->monitor);

			capture->reset_timeout = 0.0f;
		}
	}

	if (!!capture->duplicator) {
		if (capture->capture_cursor)
			cursor_capture(&capture->cursor_data);

		if (!gs_duplicator_update_frame(capture->duplicator)) {
			free_capture_data(capture);
		} else if (capture->capture.width == 0) {
			reset_capture_data(capture);
		}
	}

	obs_leave_graphics();

	if (!capture->showing)
		capture->showing = true;

	UNUSED_PARAMETER(seconds);
}

static uint32_t duplicator_capture_width(void *data)
{
	struct duplicator_capture *capture = data;
	return capture->rot % 180 == 0 ? capture->capture.width
				       : capture->capture.height;
}

static uint32_t duplicator_capture_height(void *data)
{
	struct duplicator_capture *capture = data;
	return capture->rot % 180 == 0 ? capture->capture.height
				       : capture->capture.width;
}

static void draw_cursor(struct duplicator_capture *capture)
{
	cursor_draw(&capture->cursor_data, -capture->capture.x,
		    -capture->capture.y, 1.0f, 1.0f,
		    capture->rot % 180 == 0 ? capture->capture.width
					    : capture->capture.height,
		    capture->rot % 180 == 0 ? capture->capture.height
					    : capture->capture.width);
}

static void duplicator_capture_render(void *data, gs_effect_t *effect)
{
	struct duplicator_capture *capture = data;
	gs_texture_t *texture;
	gs_eparam_t *image;
	int rot;

	if (!capture->duplicator)
		return;

	texture = gs_duplicator_get_texture(capture->duplicator);
	if (!texture)
		return;

	effect = obs_get_base_effect(OBS_EFFECT_OPAQUE);

	image = gs_effect_get_param_by_name(effect, "image");

	gs_effect_set_texture(image, texture);

	rot = capture->rot;

	while (gs_effect_loop(effect, "Draw")) {
		gs_matrix_push();

		if (rot != 0) {
			float x = 0.0f;
			float y = 0.0f;

			switch (rot) {
			case 90:
				x = (float)capture->capture.height;
				break;
			case 180:
				x = (float)capture->capture.width;
				y = (float)capture->capture.height;
				break;
			case 270:
				y = (float)capture->capture.width;
				break;
			}

			gs_matrix_translate3f(x, y, 0.0f);
			gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, RAD((float)rot));
		}

		gs_draw_sprite_subregion(texture, 0, capture->capture.x,
					 capture->capture.y,
					 capture->capture.width,
					 capture->capture.height);
		gs_matrix_pop();
	}

	if (capture->capture_cursor) {
		effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);

		while (gs_effect_loop(effect, "Draw")) {
			draw_cursor(capture);
		}
	}
}

static bool get_monitor_props(obs_property_t *monitor_list, int monitor_idx)
{
	struct dstr monitor_desc = {0};
	struct gs_monitor_info info;

	if (!gs_get_duplicator_monitor_info(monitor_idx, &info))
		return false;

	dstr_catf(&monitor_desc, "%s %d: %ldx%ld @ %ld,%ld", TEXT_MONITOR,
		  monitor_idx + 1, info.cx, info.cy, info.x, info.y);

	obs_property_list_add_int(monitor_list, monitor_desc.array,
				  monitor_idx);

	dstr_free(&monitor_desc);

	return true;
}

static obs_properties_t *duplicator_capture_properties(void *unused)
{
	int monitor_idx = 0;

	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create();

	obs_property_t *monitors = obs_properties_add_list(
		props, "monitor", TEXT_MONITOR, OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);

	obs_properties_add_bool(props, "capture_cursor", TEXT_CAPTURE_CURSOR);

	obs_properties_add_bool(props, "capture_foreground_window",
				TEXT_CAPTURE_FOREGROUND_WINDOW);

	obs_enter_graphics();

	while (get_monitor_props(monitors, monitor_idx++))
		;

	obs_leave_graphics();

	return props;
}

struct obs_source_info duplicator_capture_info = {
	.id = "monitor_capture",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW |
			OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = duplicator_capture_getname,
	.create = duplicator_capture_create,
	.destroy = duplicator_capture_destroy,
	.video_render = duplicator_capture_render,
	.video_tick = duplicator_capture_tick,
	.update = duplicator_capture_update,
	.get_width = duplicator_capture_width,
	.get_height = duplicator_capture_height,
	.get_defaults = duplicator_capture_defaults,
	.get_properties = duplicator_capture_properties,
	.icon_type = OBS_ICON_TYPE_DESKTOP_CAPTURE,
};
