/*
Planning Center Live for OBS
Copyright (C) 2026 meyer.productions

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

/*
 * Source "Planning Center Live": renders a private browser source that shows
 * the bundled overlay (data/overlay/overlay.html). The plugin polls the
 * Planning Center API itself and pushes the state into the page with the
 * browser source's "javascript_event" procedure – no web server, no cookies.
 *
 * The Planning Center connection is global (Tools -> Planning Center Live
 * settings, like the DistroAV settings) and shared by all sources; the source
 * properties only contain the view and its appearance.
 */

#include "pco-data.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <callback/calldata.h>
#include <util/platform.h>
#include <plugin-support.h>

#include "update-check.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#define SOURCE_ID "planning_center_live_source"
#define SETTINGS_ID "planning_center_live_settings"
#define T(x) obs_module_text(x)

static const char *VIEWS[] = {"lowerthird", "stage", "countdown", "list", "service", "clock", "timer", "off"};
static const size_t VIEW_COUNT = sizeof(VIEWS) / sizeof(VIEWS[0]);
static const unsigned DEFAULT_ACCENT = 0xFF2B00E4; // #e4002b in OBS ABGR

/* ------------------------------------------------------------------------ */
/* global connection (Tools menu)                                            */
/* ------------------------------------------------------------------------ */

static std::mutex g_conn_mtx;
static PcoConfig g_conn;
static int g_poll_seconds = 2;
static std::atomic<int> g_conn_gen{1};
static std::vector<std::pair<std::string, std::string>> g_service_types;
static obs_source_t *g_settings_src = nullptr;

static std::string conn_file()
{
	char *path = obs_module_config_path("connection.json");
	std::string s = path ? path : "";
	bfree(path);
	return s;
}

static PcoConfig current_conn(int *poll = nullptr)
{
	std::lock_guard<std::mutex> lock(g_conn_mtx);
	if (poll)
		*poll = g_poll_seconds;
	return g_conn;
}

void pco_load_connection_defaults()
{
	obs_data_t *d = obs_data_create_from_json_file(conn_file().c_str());
	if (!d)
		return;
	std::lock_guard<std::mutex> lock(g_conn_mtx);
	g_conn.app_id = obs_data_get_string(d, "app_id");
	g_conn.secret = obs_data_get_string(d, "secret");
	g_conn.service_type = obs_data_get_string(d, "service_type");
	g_conn.plan_id = obs_data_get_string(d, "plan_id");
	g_conn.demo = obs_data_get_bool(d, "demo");
	long long poll = obs_data_has_user_value(d, "poll_seconds") ? obs_data_get_int(d, "poll_seconds") : 2;
	g_poll_seconds = poll > 0 ? static_cast<int>(poll) : 2;
	obs_data_release(d);
}

static void save_connection()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	obs_data_t *d = obs_data_create();
	{
		std::lock_guard<std::mutex> lock(g_conn_mtx);
		obs_data_set_string(d, "app_id", g_conn.app_id.c_str());
		obs_data_set_string(d, "secret", g_conn.secret.c_str());
		obs_data_set_string(d, "service_type", g_conn.service_type.c_str());
		obs_data_set_string(d, "plan_id", g_conn.plan_id.c_str());
		obs_data_set_bool(d, "demo", g_conn.demo);
		obs_data_set_int(d, "poll_seconds", g_poll_seconds);
	}
	obs_data_save_json_safe(d, conn_file().c_str(), "tmp", "bak");
	obs_data_release(d);
}

static std::string feed_status()
{
	auto feed = PcoFeed::acquire(current_conn());
	return feed->status();
}

static void fill_service_types(obs_property_t *list, const std::string &current)
{
	obs_property_list_clear(list);
	obs_property_list_add_string(list, T("ServiceType.Choose"), "");
	bool found = current.empty();
	std::lock_guard<std::mutex> lock(g_conn_mtx);
	for (auto &st : g_service_types) {
		obs_property_list_add_string(list, st.second.c_str(), st.first.c_str());
		if (st.first == current)
			found = true;
	}
	if (!found) {
		std::string label = "ID " + current;
		obs_property_list_add_string(list, label.c_str(), current.c_str());
	}
}

/* --- settings "source" (never shown in the source list, only opened from the Tools menu) --- */

struct SettingsData {
	obs_source_t *self;
};

static const char *settings_get_name(void *)
{
	return T("Settings.Title");
}

static void settings_update(void *, obs_data_t *settings)
{
	{
		std::lock_guard<std::mutex> lock(g_conn_mtx);
		g_conn.app_id = obs_data_get_string(settings, "app_id");
		g_conn.secret = obs_data_get_string(settings, "secret");
		g_conn.service_type = obs_data_get_string(settings, "service_type");
		g_conn.plan_id = obs_data_get_string(settings, "plan_id");
		g_conn.demo = obs_data_get_bool(settings, "demo");
		g_poll_seconds = static_cast<int>(obs_data_get_int(settings, "poll_seconds"));
		if (g_poll_seconds < 1)
			g_poll_seconds = 2;
	}
	save_connection();
	g_conn_gen++; // sources pick up the new connection on their next tick
}

static void *settings_create(obs_data_t *, obs_source_t *source)
{
	SettingsData *d = new SettingsData();
	d->self = source;
	return d;
}

static void settings_destroy(void *data)
{
	delete static_cast<SettingsData *>(data);
}

static void settings_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "poll_seconds", 2);
}

static bool load_service_types_clicked(obs_properties_t *props, obs_property_t *, void *data)
{
	SettingsData *d = static_cast<SettingsData *>(data);
	if (!d)
		return false;
	// the properties dialog edits the settings object directly (deferred update),
	// so the values typed in are available here before "OK"
	obs_data_t *settings = obs_source_get_settings(d->self);
	std::string app = obs_data_get_string(settings, "app_id");
	std::string secret = obs_data_get_string(settings, "secret");
	std::string current = obs_data_get_string(settings, "service_type");
	obs_data_release(settings);

	std::string error;
	auto types = pco_service_types(app, secret, error);
	obs_property_t *status = obs_properties_get(props, "status");
	if (!error.empty()) {
		std::string msg = std::string(T("Status.Error")) + ": " + error;
		obs_property_set_description(status, msg.c_str());
		return true;
	}
	{
		std::lock_guard<std::mutex> lock(g_conn_mtx);
		g_service_types = types;
	}
	fill_service_types(obs_properties_get(props, "service_type"), current);
	std::string msg = std::to_string(types.size()) + " " + T("ServiceType.Loaded");
	obs_property_set_description(status, msg.c_str());
	return true;
}

static bool refresh_clicked(obs_properties_t *props, obs_property_t *, void *)
{
	auto feed = PcoFeed::acquire(current_conn());
	feed->refresh();
	std::string msg = std::string(T("Status")) + ": " + feed->status();
	obs_property_set_description(obs_properties_get(props, "status"), msg.c_str());
	return true;
}

static bool update_check_clicked(obs_properties_t *props, obs_property_t *, void *)
{
	UpdateInfo i = pco_update_check_now();
	obs_property_set_description(obs_properties_get(props, "update_info"), pco_update_text().c_str());
	obs_property_set_visible(obs_properties_get(props, "update_download"), i.available);
	return true;
}

static bool update_download_clicked(obs_properties_t *, obs_property_t *, void *)
{
	pco_update_open_download();
	return false;
}

static obs_properties_t *settings_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	obs_properties_add_text(props, "token_info", T("TokenInfo"), OBS_TEXT_INFO);
	obs_properties_add_text(props, "app_id", T("AppId"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "secret", T("Secret"), OBS_TEXT_PASSWORD);
	obs_property_t *st = obs_properties_add_list(props, "service_type", T("ServiceType"), OBS_COMBO_TYPE_LIST,
						     OBS_COMBO_FORMAT_STRING);
	fill_service_types(st, current_conn().service_type);
	obs_properties_add_button2(props, "load_types", T("ServiceType.Load"), load_service_types_clicked, data);
	obs_property_t *plan = obs_properties_add_text(props, "plan_id", T("PlanId"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(plan, T("PlanId.Tooltip"));
	obs_properties_add_int(props, "poll_seconds", T("PollSeconds"), 1, 30, 1);
	obs_properties_add_bool(props, "demo", T("Demo"));
	std::string status = std::string(T("Status")) + ": " + feed_status();
	obs_properties_add_text(props, "status", status.c_str(), OBS_TEXT_INFO);
	obs_properties_add_button2(props, "refresh", T("Refresh"), refresh_clicked, data);

	// updates: GitHub releases, only check + open the download
	obs_properties_add_text(props, "update_info", pco_update_text().c_str(), OBS_TEXT_INFO);
	obs_properties_add_button2(props, "update_check", T("Update.Check"), update_check_clicked, data);
	obs_property_t *dl =
		obs_properties_add_button2(props, "update_download", T("Update.Download"), update_download_clicked, data);
	obs_property_set_visible(dl, pco_update_info().available);
	return props;
}

static void open_settings(void *)
{
	if (!g_settings_src) {
		obs_data_t *s = obs_data_create();
		{
			std::lock_guard<std::mutex> lock(g_conn_mtx);
			obs_data_set_string(s, "app_id", g_conn.app_id.c_str());
			obs_data_set_string(s, "secret", g_conn.secret.c_str());
			obs_data_set_string(s, "service_type", g_conn.service_type.c_str());
			obs_data_set_string(s, "plan_id", g_conn.plan_id.c_str());
			obs_data_set_bool(s, "demo", g_conn.demo);
			obs_data_set_int(s, "poll_seconds", g_poll_seconds);
		}
		g_settings_src = obs_source_create_private(SETTINGS_ID, T("Settings.Title"), s);
		obs_data_release(s);
	}
	if (g_settings_src)
		obs_frontend_open_source_properties(g_settings_src);
}

static void frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT && g_settings_src) {
		obs_source_release(g_settings_src);
		g_settings_src = nullptr;
	}
}

/* ------------------------------------------------------------------------ */
/* source                                                                    */
/* ------------------------------------------------------------------------ */

struct PcoSource;

struct HotkeyCtx {
	PcoSource *src;
	const char *view;
};

struct PcoSource {
	obs_source_t *self = nullptr;
	obs_source_t *browser = nullptr;

	std::mutex mtx;
	std::shared_ptr<PcoFeed> feed;
	std::string feed_key;
	int feed_gen = 0;
	std::string view = "lowerthird";
	std::string config_json = "{}";
	uint32_t width = 1920, height = 1080;

	float push_timer = 0.0f;
	std::vector<obs_hotkey_id> hotkeys;
	std::vector<HotkeyCtx> hk_ctx; // reserved once, pointers stay valid
};

static const char *pco_get_name(void *)
{
	return T("PlanningCenterLive");
}

static std::string color_hex(long long abgr)
{
	char buf[8];
	snprintf(buf, sizeof(buf), "#%02x%02x%02x", (unsigned)(abgr & 0xFF), (unsigned)((abgr >> 8) & 0xFF),
		 (unsigned)((abgr >> 16) & 0xFF));
	return buf;
}

/* (re)connect the source to the feed of the current global connection */
static void sync_feed(PcoSource *s)
{
	int gen = g_conn_gen.load();
	int poll = 2;
	PcoConfig cfg = current_conn(&poll);
	std::shared_ptr<PcoFeed> old_feed; // released outside the lock
	std::lock_guard<std::mutex> lock(s->mtx);
	if (s->feed && s->feed_gen == gen)
		return;
	if (!s->feed || cfg.key() != s->feed_key) {
		old_feed = s->feed;
		s->feed = PcoFeed::acquire(cfg);
		s->feed_key = cfg.key();
	}
	s->feed->set_interval(poll);
	s->feed_gen = gen;
}

static void update_browser_size(PcoSource *s)
{
	if (!s->browser)
		return;
	obs_data_t *bs = obs_data_create();
	obs_data_set_int(bs, "width", s->width);
	obs_data_set_int(bs, "height", s->height);
	obs_source_update(s->browser, bs);
	obs_data_release(bs);
}

static void push_state(PcoSource *s)
{
	if (!s->browser)
		return;
	std::string state, view, config;
	{
		std::lock_guard<std::mutex> lock(s->mtx);
		if (!s->feed)
			return;
		state = s->feed->state_json();
		view = s->view;
		config = s->config_json;
	}
	std::string payload = "{\"state\":" + state + ",\"view\":\"" + view + "\",\"config\":" + config +
			      ",\"now\":" + std::to_string(pco_now_ms()) + "}";

	proc_handler_t *ph = obs_source_get_proc_handler(s->browser);
	if (!ph)
		return;
	calldata_t cd;
	calldata_init(&cd);
	calldata_set_string(&cd, "eventName", "pcoLiveState");
	calldata_set_string(&cd, "jsonString", payload.c_str());
	proc_handler_call(ph, "javascript_event", &cd);
	calldata_free(&cd);
}

static void pco_update(void *data, obs_data_t *settings)
{
	PcoSource *s = static_cast<PcoSource *>(data);

	obs_data_t *c = obs_data_create();
	obs_data_set_string(c, "accent", color_hex(obs_data_get_int(settings, "accent")).c_str());
	obs_data_set_int(c, "warn_seconds", obs_data_get_int(settings, "warn_seconds"));
	obs_data_set_bool(c, "show_next", obs_data_get_bool(settings, "show_next"));
	obs_data_set_bool(c, "show_progress", obs_data_get_bool(settings, "show_progress"));
	obs_data_set_int(c, "preservice_minutes", obs_data_get_int(settings, "preservice_minutes"));
	obs_data_set_int(c, "lt_bottom", obs_data_get_int(settings, "lt_bottom"));
	obs_data_set_int(c, "lt_side", obs_data_get_int(settings, "lt_side"));
	obs_data_set_int(c, "lt_width", obs_data_get_int(settings, "lt_width"));
	obs_data_set_int(c, "lt_height", obs_data_get_int(settings, "lt_height"));
	obs_data_set_bool(c, "lt_clock", obs_data_get_bool(settings, "lt_clock"));
	obs_data_set_int(c, "stage_height", obs_data_get_int(settings, "stage_height"));
	obs_data_set_bool(c, "stage_light", obs_data_get_bool(settings, "stage_light"));
	obs_data_set_bool(c, "stage_seconds", obs_data_get_bool(settings, "stage_seconds"));
	obs_data_set_bool(c, "timer_clock", obs_data_get_bool(settings, "timer_clock"));
	obs_data_set_bool(c, "opaque", obs_data_get_bool(settings, "opaque"));
	obs_data_set_string(c, "lang", obs_data_get_string(settings, "lang"));
	std::string config_json = obs_data_get_json(c);
	obs_data_release(c);

	{
		std::lock_guard<std::mutex> lock(s->mtx);
		s->view = obs_data_get_string(settings, "view");
		if (s->view.empty())
			s->view = "lowerthird";
		s->config_json = config_json;
	}
	sync_feed(s);

	uint32_t w = static_cast<uint32_t>(obs_data_get_int(settings, "width"));
	uint32_t h = static_cast<uint32_t>(obs_data_get_int(settings, "height"));
	if (w != s->width || h != s->height) {
		s->width = w ? w : 1920;
		s->height = h ? h : 1080;
		update_browser_size(s);
	}
	push_state(s);
}

static void set_view(PcoSource *s, const std::string &view)
{
	std::string target = view;
	if (view == "next") {
		size_t idx = 0;
		{
			std::lock_guard<std::mutex> lock(s->mtx);
			for (size_t i = 0; i < VIEW_COUNT; i++)
				if (s->view == VIEWS[i])
					idx = i;
		}
		target = VIEWS[(idx + 1) % VIEW_COUNT];
	}
	obs_data_t *d = obs_data_create();
	obs_data_set_string(d, "view", target.c_str());
	obs_source_update(s->self, d); // persists the choice and calls pco_update
	obs_data_release(d);
}

static void toggle_stage_light(PcoSource *s)
{
	obs_data_t *cur = obs_source_get_settings(s->self);
	bool light = obs_data_get_bool(cur, "stage_light");
	obs_data_release(cur);
	obs_data_t *d = obs_data_create();
	obs_data_set_bool(d, "stage_light", !light);
	obs_source_update(s->self, d);
	obs_data_release(d);
}

static void hotkey_cb(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	HotkeyCtx *hc = static_cast<HotkeyCtx *>(data);
	std::string action = hc->view;
	if (action == "action.stage_light")
		toggle_stage_light(hc->src);
	else
		set_view(hc->src, hc->view);
}

static void *pco_create(obs_data_t *settings, obs_source_t *source)
{
	PcoSource *s = new PcoSource();
	s->self = source;
	s->width = static_cast<uint32_t>(obs_data_get_int(settings, "width"));
	s->height = static_cast<uint32_t>(obs_data_get_int(settings, "height"));
	if (!s->width)
		s->width = 1920;
	if (!s->height)
		s->height = 1080;

	char *file = obs_module_file("overlay/overlay.html");
	obs_data_t *bs = obs_data_create();
	obs_data_set_bool(bs, "is_local_file", true);
	obs_data_set_string(bs, "local_file", file ? file : "");
	obs_data_set_int(bs, "width", s->width);
	obs_data_set_int(bs, "height", s->height);
	obs_data_set_string(bs, "css", "");
	obs_data_set_bool(bs, "shutdown", false);
	obs_data_set_bool(bs, "restart_when_active", false);
	bfree(file);
	s->browser = obs_source_create_private("browser_source", "Planning Center Live (browser)", bs);
	obs_data_release(bs);
	if (s->browser)
		obs_source_add_active_child(source, s->browser);
	else
		obs_log(LOG_ERROR, "browser_source not available – is obs-browser installed?");

	// hotkeys: one per view + "next view"
	s->hk_ctx.reserve(VIEW_COUNT + 2);
	s->hk_ctx.push_back({s, "next"});
	for (size_t i = 0; i < VIEW_COUNT; i++)
		s->hk_ctx.push_back({s, VIEWS[i]});
	s->hk_ctx.push_back({s, "action.stage_light"});
	for (HotkeyCtx &hc : s->hk_ctx) {
		std::string id = hc.view;
		bool action = id.rfind("action.", 0) == 0;
		std::string name = action ? "pco_live." + id : "pco_live.view." + id;
		std::string key = std::string("Hotkey.") + (action ? id.substr(7) : id);
		s->hotkeys.push_back(obs_hotkey_register_source(source, name.c_str(), T(key.c_str()), hotkey_cb, &hc));
	}

	pco_update(s, settings);
	return s;
}

static void pco_destroy(void *data)
{
	PcoSource *s = static_cast<PcoSource *>(data);
	for (obs_hotkey_id id : s->hotkeys)
		obs_hotkey_unregister(id);
	if (s->browser) {
		obs_source_remove_active_child(s->self, s->browser);
		obs_source_release(s->browser);
	}
	delete s;
}

static uint32_t pco_width(void *data)
{
	return static_cast<PcoSource *>(data)->width;
}

static uint32_t pco_height(void *data)
{
	return static_cast<PcoSource *>(data)->height;
}

static void pco_render(void *data, gs_effect_t *)
{
	PcoSource *s = static_cast<PcoSource *>(data);
	if (s->browser)
		obs_source_video_render(s->browser);
}

static void pco_tick(void *data, float seconds)
{
	PcoSource *s = static_cast<PcoSource *>(data);
	s->push_timer += seconds;
	if (s->push_timer >= 0.5f) {
		s->push_timer = 0.0f;
		sync_feed(s); // connection may have been changed in the Tools menu
		push_state(s);
	}
}

static void pco_enum_sources(void *data, obs_source_enum_proc_t cb, void *param)
{
	PcoSource *s = static_cast<PcoSource *>(data);
	if (s->browser)
		cb(s->self, s->browser, param);
}

static void pco_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "view", "lowerthird");
	obs_data_set_default_int(settings, "accent", DEFAULT_ACCENT);
	obs_data_set_default_int(settings, "warn_seconds", 30);
	obs_data_set_default_bool(settings, "show_next", true);
	obs_data_set_default_bool(settings, "show_progress", true);
	obs_data_set_default_int(settings, "preservice_minutes", 30);
	obs_data_set_default_int(settings, "lt_bottom", 16);
	obs_data_set_default_int(settings, "lt_side", 24);
	obs_data_set_default_int(settings, "lt_width", 100);
	obs_data_set_default_int(settings, "lt_height", 64);
	obs_data_set_default_bool(settings, "lt_clock", true);
	obs_data_set_default_int(settings, "stage_height", 240);
	obs_data_set_default_bool(settings, "stage_light", false);
	obs_data_set_default_bool(settings, "stage_seconds", true);
	obs_data_set_default_bool(settings, "timer_clock", true);
	obs_data_set_default_bool(settings, "opaque", false);
	obs_data_set_default_string(settings, "lang", "de");
	obs_data_set_default_int(settings, "width", 1920);
	obs_data_set_default_int(settings, "height", 1080);
}

static obs_properties_t *pco_properties(void *data)
{
	PcoSource *s = static_cast<PcoSource *>(data);
	obs_properties_t *props = obs_properties_create();

	obs_property_t *view = obs_properties_add_list(props, "view", T("View"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	for (size_t i = 0; i < VIEW_COUNT; i++) {
		std::string key = std::string("View.") + VIEWS[i];
		obs_property_list_add_string(view, T(key.c_str()), VIEWS[i]);
	}

	// lower third
	obs_properties_t *lt = obs_properties_create();
	obs_properties_add_int_slider(lt, "lt_width", T("LowerThirdWidth"), 20, 100, 1);
	obs_properties_add_int_slider(lt, "lt_height", T("LowerThirdHeight"), 40, 160, 1);
	obs_properties_add_int_slider(lt, "lt_bottom", T("LowerThirdBottom"), 0, 300, 1);
	obs_properties_add_int_slider(lt, "lt_side", T("LowerThirdSide"), 0, 400, 1);
	obs_properties_add_bool(lt, "lt_clock", T("LowerThirdClock"));
	obs_properties_add_group(props, "lowerthird", T("Group.LowerThird"), OBS_GROUP_NORMAL, lt);

	// stage banner (uses distance bottom/sides of the lower third)
	obs_properties_t *sb = obs_properties_create();
	obs_properties_add_int_slider(sb, "stage_height", T("StageHeight"), 120, 480, 1);
	obs_properties_add_bool(sb, "stage_light", T("StageLight"));
	obs_properties_add_bool(sb, "stage_seconds", T("StageSeconds"));
	obs_properties_add_group(props, "stage", T("Group.Stage"), OBS_GROUP_NORMAL, sb);

	// timer + clock
	obs_properties_t *tmg = obs_properties_create();
	obs_properties_add_bool(tmg, "timer_clock", T("TimerClock"));
	obs_properties_add_group(props, "timer", T("Group.Timer"), OBS_GROUP_NORMAL, tmg);

	// appearance
	obs_properties_t *ap = obs_properties_create();
	obs_properties_add_color(ap, "accent", T("Accent"));
	obs_properties_add_int_slider(ap, "warn_seconds", T("WarnSeconds"), 0, 300, 5);
	obs_properties_add_bool(ap, "show_next", T("ShowNext"));
	obs_properties_add_bool(ap, "show_progress", T("ShowProgress"));
	obs_properties_add_int(ap, "preservice_minutes", T("PreserviceMinutes"), 0, 240, 5);
	obs_properties_add_bool(ap, "opaque", T("Opaque"));
	obs_property_t *lang = obs_properties_add_list(ap, "lang", T("Language"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(lang, "Deutsch", "de");
	obs_property_list_add_string(lang, "English", "en");
	obs_properties_add_int(ap, "width", T("Width"), 320, 7680, 1);
	obs_properties_add_int(ap, "height", T("Height"), 180, 4320, 1);
	obs_properties_add_group(props, "appearance", T("Group.Appearance"), OBS_GROUP_NORMAL, ap);

	// connection: only a hint + status, settings live in the Tools menu
	std::string status = std::string(T("Status")) + ": ";
	if (s) {
		std::lock_guard<std::mutex> lock(s->mtx);
		if (s->feed)
			status += s->feed->status();
	}
	obs_properties_add_text(props, "connection_info", T("ConnectionInfo"), OBS_TEXT_INFO);
	obs_properties_add_text(props, "status", status.c_str(), OBS_TEXT_INFO);
	if (pco_update_info().available) {
		std::string upd = pco_update_text() + " – " + T("Update.Hint");
		obs_properties_add_text(props, "update_hint", upd.c_str(), OBS_TEXT_INFO);
	}
	return props;
}

void pco_register_source()
{
	struct obs_source_info info = {};
	info.id = SOURCE_ID;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	info.icon_type = OBS_ICON_TYPE_BROWSER;
	info.get_name = pco_get_name;
	info.create = pco_create;
	info.destroy = pco_destroy;
	info.update = pco_update;
	info.get_defaults = pco_defaults;
	info.get_properties = pco_properties;
	info.get_width = pco_width;
	info.get_height = pco_height;
	info.video_render = pco_render;
	info.video_tick = pco_tick;
	info.enum_active_sources = pco_enum_sources;
	obs_register_source(&info);

	// hidden settings type, opened from Tools -> Planning Center Live settings
	struct obs_source_info sinfo = {};
	sinfo.id = SETTINGS_ID;
	sinfo.type = OBS_SOURCE_TYPE_INPUT;
	sinfo.output_flags = OBS_SOURCE_CAP_OBSOLETE; // not offered in "Add source"
	sinfo.get_name = settings_get_name;
	sinfo.create = settings_create;
	sinfo.destroy = settings_destroy;
	sinfo.update = settings_update;
	sinfo.get_defaults = settings_defaults;
	sinfo.get_properties = settings_properties;
	obs_register_source(&sinfo);
}

void pco_register_frontend()
{
	obs_frontend_add_tools_menu_item(T("Menu.Settings"), open_settings, nullptr);
	obs_frontend_add_event_callback(frontend_event, nullptr);
}
