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
 */

#include "pco-data.hpp"

#include <obs-module.h>
#include <callback/calldata.h>
#include <util/platform.h>
#include <plugin-support.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#define SOURCE_ID "planning_center_live_source"
#define T(x) obs_module_text(x)

static const char *VIEWS[] = {"lowerthird", "countdown", "list", "service", "clock", "off"};
static const size_t VIEW_COUNT = sizeof(VIEWS) / sizeof(VIEWS[0]);
static const unsigned DEFAULT_ACCENT = 0xFF2B00E4; // #e4002b in OBS ABGR

/* ------------------------------------------------------------------------ */
/* shared connection defaults (so a second source does not need the token again) */
/* ------------------------------------------------------------------------ */

static std::mutex g_conn_mtx;
static std::string g_app_id, g_secret, g_service_type;
static std::vector<std::pair<std::string, std::string>> g_service_types;

static std::string conn_file()
{
	char *path = obs_module_config_path("connection.json");
	std::string s = path ? path : "";
	bfree(path);
	return s;
}

void pco_load_connection_defaults()
{
	obs_data_t *d = obs_data_create_from_json_file(conn_file().c_str());
	if (!d)
		return;
	std::lock_guard<std::mutex> lock(g_conn_mtx);
	g_app_id = obs_data_get_string(d, "app_id");
	g_secret = obs_data_get_string(d, "secret");
	g_service_type = obs_data_get_string(d, "service_type");
	obs_data_release(d);
}

static void save_connection_defaults(const PcoConfig &cfg)
{
	std::lock_guard<std::mutex> lock(g_conn_mtx);
	if (cfg.app_id == g_app_id && cfg.secret == g_secret && cfg.service_type == g_service_type)
		return;
	g_app_id = cfg.app_id;
	g_secret = cfg.secret;
	g_service_type = cfg.service_type;
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	obs_data_t *d = obs_data_create();
	obs_data_set_string(d, "app_id", g_app_id.c_str());
	obs_data_set_string(d, "secret", g_secret.c_str());
	obs_data_set_string(d, "service_type", g_service_type.c_str());
	obs_data_save_json_safe(d, conn_file().c_str(), "tmp", "bak");
	obs_data_release(d);
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

	PcoConfig cfg;
	cfg.app_id = obs_data_get_string(settings, "app_id");
	cfg.secret = obs_data_get_string(settings, "secret");
	cfg.service_type = obs_data_get_string(settings, "service_type");
	cfg.plan_id = obs_data_get_string(settings, "plan_id");
	cfg.demo = obs_data_get_bool(settings, "demo");
	if (cfg.has_credentials() && !cfg.demo)
		save_connection_defaults(cfg);

	obs_data_t *c = obs_data_create();
	obs_data_set_string(c, "accent", color_hex(obs_data_get_int(settings, "accent")).c_str());
	obs_data_set_int(c, "warn_seconds", obs_data_get_int(settings, "warn_seconds"));
	obs_data_set_bool(c, "show_next", obs_data_get_bool(settings, "show_next"));
	obs_data_set_bool(c, "show_progress", obs_data_get_bool(settings, "show_progress"));
	obs_data_set_int(c, "preservice_minutes", obs_data_get_int(settings, "preservice_minutes"));
	obs_data_set_int(c, "lt_bottom", obs_data_get_int(settings, "lt_bottom"));
	obs_data_set_int(c, "lt_side", obs_data_get_int(settings, "lt_side"));
	obs_data_set_bool(c, "opaque", obs_data_get_bool(settings, "opaque"));
	obs_data_set_string(c, "lang", obs_data_get_string(settings, "lang"));
	std::string config_json = obs_data_get_json(c);
	obs_data_release(c);

	std::shared_ptr<PcoFeed> old_feed;
	{
		std::lock_guard<std::mutex> lock(s->mtx);
		s->view = obs_data_get_string(settings, "view");
		if (s->view.empty())
			s->view = "lowerthird";
		s->config_json = config_json;
		if (cfg.key() != s->feed_key || !s->feed) {
			old_feed = s->feed;
			s->feed = PcoFeed::acquire(cfg);
			s->feed_key = cfg.key();
		}
		s->feed->set_interval(static_cast<int>(obs_data_get_int(settings, "poll_seconds")));
	}

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

static void hotkey_cb(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	HotkeyCtx *hc = static_cast<HotkeyCtx *>(data);
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
	s->hk_ctx.reserve(VIEW_COUNT + 1);
	s->hk_ctx.push_back({s, "next"});
	for (size_t i = 0; i < VIEW_COUNT; i++)
		s->hk_ctx.push_back({s, VIEWS[i]});
	for (HotkeyCtx &hc : s->hk_ctx) {
		std::string name = std::string("pco_live.view.") + hc.view;
		std::string key = std::string("Hotkey.") + hc.view;
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
	obs_data_set_default_int(settings, "lt_side", 40);
	obs_data_set_default_bool(settings, "opaque", false);
	obs_data_set_default_string(settings, "lang", "de");
	obs_data_set_default_int(settings, "width", 1920);
	obs_data_set_default_int(settings, "height", 1080);
	obs_data_set_default_int(settings, "poll_seconds", 2);

	std::lock_guard<std::mutex> lock(g_conn_mtx);
	obs_data_set_default_string(settings, "app_id", g_app_id.c_str());
	obs_data_set_default_string(settings, "secret", g_secret.c_str());
	obs_data_set_default_string(settings, "service_type", g_service_type.c_str());
}

/* ------------------------------------------------------------------------ */
/* properties                                                                */
/* ------------------------------------------------------------------------ */

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

static bool load_service_types_clicked(obs_properties_t *props, obs_property_t *, void *data)
{
	PcoSource *s = static_cast<PcoSource *>(data);
	obs_data_t *settings = obs_source_get_settings(s->self);
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

static bool refresh_clicked(obs_properties_t *props, obs_property_t *, void *data)
{
	PcoSource *s = static_cast<PcoSource *>(data);
	std::string status;
	{
		std::lock_guard<std::mutex> lock(s->mtx);
		if (s->feed) {
			s->feed->refresh();
			status = s->feed->status();
		}
	}
	std::string msg = std::string(T("Status")) + ": " + status;
	obs_property_set_description(obs_properties_get(props, "status"), msg.c_str());
	return true;
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

	// appearance
	obs_properties_t *ap = obs_properties_create();
	obs_properties_add_color(ap, "accent", T("Accent"));
	obs_properties_add_int_slider(ap, "warn_seconds", T("WarnSeconds"), 0, 300, 5);
	obs_properties_add_bool(ap, "show_next", T("ShowNext"));
	obs_properties_add_bool(ap, "show_progress", T("ShowProgress"));
	obs_properties_add_int(ap, "preservice_minutes", T("PreserviceMinutes"), 0, 240, 5);
	obs_properties_add_int_slider(ap, "lt_bottom", T("LowerThirdBottom"), 0, 300, 1);
	obs_properties_add_int_slider(ap, "lt_side", T("LowerThirdSide"), 0, 400, 1);
	obs_properties_add_bool(ap, "opaque", T("Opaque"));
	obs_property_t *lang = obs_properties_add_list(ap, "lang", T("Language"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(lang, "Deutsch", "de");
	obs_property_list_add_string(lang, "English", "en");
	obs_properties_add_int(ap, "width", T("Width"), 320, 7680, 1);
	obs_properties_add_int(ap, "height", T("Height"), 180, 4320, 1);
	obs_properties_add_group(props, "appearance", T("Group.Appearance"), OBS_GROUP_NORMAL, ap);

	// connection
	obs_properties_t *cp = obs_properties_create();
	obs_properties_add_text(cp, "token_info", T("TokenInfo"), OBS_TEXT_INFO);
	obs_properties_add_text(cp, "app_id", T("AppId"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(cp, "secret", T("Secret"), OBS_TEXT_PASSWORD);
	obs_property_t *st = obs_properties_add_list(cp, "service_type", T("ServiceType"), OBS_COMBO_TYPE_LIST,
						     OBS_COMBO_FORMAT_STRING);
	{
		obs_data_t *settings = s ? obs_source_get_settings(s->self) : nullptr;
		std::string current = settings ? obs_data_get_string(settings, "service_type") : "";
		obs_data_release(settings);
		fill_service_types(st, current);
	}
	obs_properties_add_button(cp, "load_types", T("ServiceType.Load"), load_service_types_clicked);
	obs_property_t *plan = obs_properties_add_text(cp, "plan_id", T("PlanId"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(plan, T("PlanId.Tooltip"));
	obs_properties_add_int(cp, "poll_seconds", T("PollSeconds"), 1, 30, 1);
	obs_properties_add_bool(cp, "demo", T("Demo"));
	std::string status = std::string(T("Status")) + ": ";
	if (s) {
		std::lock_guard<std::mutex> lock(s->mtx);
		if (s->feed)
			status += s->feed->status();
	}
	obs_properties_add_text(cp, "status", status.c_str(), OBS_TEXT_INFO);
	obs_properties_add_button(cp, "refresh", T("Refresh"), refresh_clicked);
	obs_properties_add_group(props, "connection", T("Group.Connection"), OBS_GROUP_NORMAL, cp);

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
}
