/*
Planning Center Live for OBS
Copyright (C) 2026 meyer.productions

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "pco-data.hpp"
#include "http-client.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <map>
#include <mutex>
#include <thread>

static const char *API_BASE = "https://api.planningcenteronline.com/services/v2";
static const int HTTP_TIMEOUT_MS = 8000;
static const long long PLAN_STICKY_MS = 3LL * 3600 * 1000; // keep a finished plan for 3 h
static const double PLAN_RECHECK_S = 300.0;
static const double ITEMS_RELOAD_S = 30.0;

static std::atomic<int> g_running_threads{0};

/* ------------------------------------------------------------------------ */
/* small RAII helpers for obs_data                                           */
/* ------------------------------------------------------------------------ */

struct OData {
	obs_data_t *p = nullptr;
	OData() = default;
	explicit OData(obs_data_t *d) : p(d) {}
	~OData()
	{
		if (p)
			obs_data_release(p);
	}
	OData(const OData &) = delete;
	OData &operator=(const OData &) = delete;
	OData(OData &&o) noexcept : p(o.p) { o.p = nullptr; }
	OData &operator=(OData &&o) noexcept
	{
		if (this != &o) {
			if (p)
				obs_data_release(p);
			p = o.p;
			o.p = nullptr;
		}
		return *this;
	}
	operator obs_data_t *() const { return p; }
	explicit operator bool() const { return p != nullptr; }
};

struct OArray {
	obs_data_array_t *p = nullptr;
	explicit OArray(obs_data_array_t *a) : p(a) {}
	~OArray()
	{
		if (p)
			obs_data_array_release(p);
	}
	OArray(const OArray &) = delete;
	OArray &operator=(const OArray &) = delete;
	size_t count() const { return p ? obs_data_array_count(p) : 0; }
	OData at(size_t i) const { return OData(obs_data_array_item(p, i)); }
};

static std::string str(obs_data_t *d, const char *name)
{
	const char *s = d ? obs_data_get_string(d, name) : nullptr;
	return s ? std::string(s) : std::string();
}

static long long num(obs_data_t *d, const char *name)
{
	return d ? obs_data_get_int(d, name) : 0;
}

/* ------------------------------------------------------------------------ */
/* time + encoding helpers                                                   */
/* ------------------------------------------------------------------------ */

long long pco_now_ms()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

/* ISO-8601 ("2026-11-08T09:31:47Z" or with +hh:mm offset) -> epoch ms, 0 if empty/invalid */
static long long parse_ts(const std::string &s)
{
	if (s.size() < 19)
		return 0;
	int y, mo, d, h, mi, se;
	if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6)
		return 0;
	struct tm t = {};
	t.tm_year = y - 1900;
	t.tm_mon = mo - 1;
	t.tm_mday = d;
	t.tm_hour = h;
	t.tm_min = mi;
	t.tm_sec = se;
#ifdef _WIN32
	long long epoch = static_cast<long long>(_mkgmtime(&t));
#else
	long long epoch = static_cast<long long>(timegm(&t));
#endif
	// timezone offset
	size_t pos = 19;
	if (pos < s.size() && s[pos] == '.') {
		while (pos < s.size() && s[pos] != 'Z' && s[pos] != '+' && s[pos] != '-')
			pos++;
	}
	if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) {
		int oh = 0, om = 0;
		if (sscanf(s.c_str() + pos + 1, "%d:%d", &oh, &om) >= 1) {
			long long off = (oh * 60LL + om) * 60LL;
			epoch += s[pos] == '+' ? -off : off;
		}
	}
	return epoch * 1000;
}

static std::string base64(const std::string &in)
{
	static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	size_t i = 0;
	while (i + 2 < in.size()) {
		unsigned n = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8 | (unsigned char)in[i + 2];
		out += tbl[(n >> 18) & 63];
		out += tbl[(n >> 12) & 63];
		out += tbl[(n >> 6) & 63];
		out += tbl[n & 63];
		i += 3;
	}
	if (i + 1 == in.size()) {
		unsigned n = (unsigned char)in[i] << 16;
		out += tbl[(n >> 18) & 63];
		out += tbl[(n >> 12) & 63];
		out += "==";
	} else if (i + 2 == in.size()) {
		unsigned n = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8;
		out += tbl[(n >> 18) & 63];
		out += tbl[(n >> 12) & 63];
		out += tbl[(n >> 6) & 63];
		out += '=';
	}
	return out;
}

std::string PcoConfig::key() const
{
	if (demo)
		return "demo";
	return app_id + "|" + secret + "|" + service_type + "|" + plan_id;
}

/* ------------------------------------------------------------------------ */
/* API access                                                                */
/* ------------------------------------------------------------------------ */

struct ApiError {
	std::string message;
	long status;
};

/* GET path (relative to API_BASE) or absolute URL; throws ApiError */
static OData api_get(const PcoConfig &cfg, const std::string &path)
{
	std::string url = path.rfind("http", 0) == 0 ? path : std::string(API_BASE) + path;
	std::string headers = "Authorization: Basic " + base64(cfg.app_id + ":" + cfg.secret) +
			      "\r\nAccept: application/json\r\n";
	HttpResponse r = http_get(url, headers, HTTP_TIMEOUT_MS);
	if (!r.error.empty() && r.status == 0)
		throw ApiError{"No connection: " + r.error, 0};
	if (r.status == 401)
		throw ApiError{"Invalid credentials (401)", 401};
	if (r.status == 429)
		throw ApiError{"Rate limit reached (429)", 429};
	if (r.status < 200 || r.status >= 300) {
		std::string detail;
		OData body(obs_data_create_from_json(r.body.c_str()));
		if (body) {
			OArray errs(obs_data_get_array(body, "errors"));
			if (errs.count()) {
				OData e = errs.at(0);
				detail = str(e, "detail");
				if (detail.empty())
					detail = str(e, "title");
			}
		}
		throw ApiError{"HTTP " + std::to_string(r.status) + (detail.empty() ? "" : " " + detail), r.status};
	}
	OData body(obs_data_create_from_json(r.body.c_str()));
	if (!body)
		throw ApiError{"Invalid JSON response", r.status};
	return body;
}

/* GET with JSON:API pagination; calls fn(resource) for every element of "data" */
template<typename Fn> static void api_get_all(const PcoConfig &cfg, std::string path, Fn fn, int max_pages = 10)
{
	for (int page = 0; page < max_pages && !path.empty(); page++) {
		OData body = api_get(cfg, path);
		OArray data(obs_data_get_array(body, "data"));
		for (size_t i = 0; i < data.count(); i++) {
			OData res = data.at(i);
			fn(res.p);
		}
		OData links(obs_data_get_obj(body, "links"));
		path = str(links, "next");
	}
}

static std::string rel_id(obs_data_t *res, const char *name)
{
	OData rels(obs_data_get_obj(res, "relationships"));
	if (!rels)
		return std::string();
	OData rel(obs_data_get_obj(rels, name));
	if (!rel)
		return std::string();
	OData d(obs_data_get_obj(rel, "data"));
	return str(d, "id");
}

std::vector<std::pair<std::string, std::string>> pco_service_types(const std::string &app_id,
								    const std::string &secret, std::string &error)
{
	std::vector<std::pair<std::string, std::string>> out;
	PcoConfig cfg;
	cfg.app_id = app_id;
	cfg.secret = secret;
	try {
		api_get_all(cfg, "/service_types?per_page=100&order=sequence", [&](obs_data_t *res) {
			OData a(obs_data_get_obj(res, "attributes"));
			out.emplace_back(str(res, "id"), str(a, "name"));
		});
	} catch (const ApiError &e) {
		error = e.message;
	}
	return out;
}

/* ------------------------------------------------------------------------ */
/* model                                                                     */
/* ------------------------------------------------------------------------ */

struct Plan {
	std::string id, title, series_title, dates;
	long long total_length = 0;
	long long last_time_at = 0;
};

struct Item {
	std::string id, title, type, position, key, description;
	long long length = 0, sequence = 0;
};

struct PlanTime {
	std::string id, name, type;
	long long starts_at = 0, ends_at = 0, live_starts_at = 0, live_ends_at = 0;
};

struct ItemTime {
	std::string id, item_id, plan_time_id;
	long long live_start_at = 0, live_end_at = 0, length = 0;
};

static Plan parse_plan(obs_data_t *res)
{
	Plan p;
	OData a(obs_data_get_obj(res, "attributes"));
	p.id = str(res, "id");
	p.title = str(a, "title");
	p.series_title = str(a, "series_title");
	p.dates = str(a, "dates");
	p.total_length = num(a, "total_length");
	p.last_time_at = parse_ts(str(a, "last_time_at"));
	return p;
}

/* ------------------------------------------------------------------------ */
/* feed                                                                      */
/* ------------------------------------------------------------------------ */

struct FeedContext {
	PcoConfig cfg;
	std::mutex mtx;
	std::condition_variable cv;
	bool stop = false;
	bool force = false;
	bool reset_caches = false;
	int interval = 2;

	std::string state = "{\"ok\":false,\"error\":\"Loading...\"}";
	std::string status = "Loading...";

	// caches (worker thread only)
	bool have_plan = false;
	Plan plan;
	double plan_checked = -1e9;
	double items_loaded = -1e9;
	std::vector<Item> items;
	std::vector<PlanTime> plan_times;
	long long demo_t0 = 0;
};

static double mono_s()
{
	using namespace std::chrono;
	return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

static void set_json_ts(obs_data_t *d, const char *name, long long ms)
{
	if (ms)
		obs_data_set_int(d, name, ms);
}

static OData plan_time_obj(const PlanTime &t)
{
	OData o(obs_data_create());
	obs_data_set_string(o, "id", t.id.c_str());
	obs_data_set_string(o, "name", t.name.c_str());
	obs_data_set_string(o, "type", t.type.c_str());
	set_json_ts(o, "starts_at", t.starts_at);
	set_json_ts(o, "ends_at", t.ends_at);
	set_json_ts(o, "live_starts_at", t.live_starts_at);
	set_json_ts(o, "live_ends_at", t.live_ends_at);
	return o;
}

static std::string build_state(const Plan *plan, const std::vector<Item> &items,
			       const std::vector<PlanTime> &plan_times, const ItemTime *cur_it, const ItemTime *next_it)
{
	OData root(obs_data_create());
	obs_data_set_bool(root, "ok", true);
	if (!plan) {
		obs_data_set_string(root, "status", "no_plan");
		return obs_data_get_json(root);
	}

	OData jp(obs_data_create());
	obs_data_set_string(jp, "id", plan->id.c_str());
	obs_data_set_string(jp, "title", plan->title.c_str());
	obs_data_set_string(jp, "series_title", plan->series_title.c_str());
	obs_data_set_string(jp, "dates", plan->dates.c_str());
	obs_data_set_int(jp, "total_length", plan->total_length);
	obs_data_set_obj(root, "plan", jp);

	std::map<std::string, const Item *> by_id;
	for (const Item &i : items)
		by_id[i.id] = &i;

	std::vector<PlanTime> services;
	for (const PlanTime &t : plan_times)
		if (t.type == "service")
			services.push_back(t);
	if (services.empty())
		services = plan_times;
	std::sort(services.begin(), services.end(),
		  [](const PlanTime &a, const PlanTime &b) { return a.starts_at < b.starts_at; });

	long long now = pco_now_ms();
	const PlanTime *running = nullptr;
	if (cur_it && !cur_it->plan_time_id.empty())
		for (const PlanTime &t : services)
			if (t.id == cur_it->plan_time_id)
				running = &t;
	if (!running)
		for (const PlanTime &t : services)
			if (t.live_starts_at && !t.live_ends_at) {
				running = &t;
				break;
			}
	const PlanTime *upcoming = nullptr;
	for (const PlanTime &t : services)
		if (t.starts_at > now) {
			upcoming = &t;
			break;
		}

	auto item_obj = [&](const ItemTime *it, bool with_timing) -> OData {
		OData o(obs_data_create());
		const Item *item = by_id.count(it->item_id) ? by_id[it->item_id] : nullptr;
		long long length = it->length ? it->length : (item ? item->length : 0);
		obs_data_set_string(o, "id", item ? item->id.c_str() : "");
		obs_data_set_string(o, "title", item ? item->title.c_str() : "");
		obs_data_set_string(o, "type", item ? item->type.c_str() : "");
		obs_data_set_string(o, "key", item ? item->key.c_str() : "");
		obs_data_set_string(o, "description", item ? item->description.c_str() : "");
		obs_data_set_int(o, "length", length);
		if (with_timing) {
			set_json_ts(o, "start", it->live_start_at);
			set_json_ts(o, "end", it->live_end_at);
		}
		return o;
	};

	OData live(obs_data_create());
	obs_data_set_bool(live, "active", cur_it != nullptr);
	if (cur_it) {
		OData c = item_obj(cur_it, true);
		obs_data_set_obj(live, "current", c);
	}
	if (next_it) {
		OData n = item_obj(next_it, false);
		obs_data_set_obj(live, "next", n);
	} else if (cur_it) {
		// fallback: following non-header item in sequence
		bool found = false;
		for (const Item &i : items) {
			if (found && i.type != "header") {
				OData n(obs_data_create());
				obs_data_set_string(n, "id", i.id.c_str());
				obs_data_set_string(n, "title", i.title.c_str());
				obs_data_set_string(n, "type", i.type.c_str());
				obs_data_set_int(n, "length", i.length);
				obs_data_set_obj(live, "next", n);
				break;
			}
			if (i.id == cur_it->item_id)
				found = true;
		}
	}
	if (running) {
		OData r = plan_time_obj(*running);
		obs_data_set_obj(live, "service", r);
	}
	if (upcoming) {
		OData u = plan_time_obj(*upcoming);
		obs_data_set_obj(live, "upcoming_service", u);
	}
	obs_data_set_obj(root, "live", live);

	// run sheet with planned start times; "pre" items run before the service start
	long long cur_seq = -1;
	if (cur_it && by_id.count(cur_it->item_id))
		cur_seq = by_id[cur_it->item_id]->sequence;
	const PlanTime *ref = running ? running : (upcoming ? upcoming : (services.empty() ? nullptr : &services[0]));
	long long planned = ref ? ref->starts_at : 0;
	if (planned) {
		long long pre = 0;
		for (const Item &i : items)
			if (i.position == "pre" && i.type != "header")
				pre += i.length;
		planned -= pre * 1000;
	}
	OArray arr(obs_data_array_create());
	for (const Item &i : items) {
		OData o(obs_data_create());
		obs_data_set_string(o, "id", i.id.c_str());
		obs_data_set_string(o, "title", i.title.c_str());
		obs_data_set_string(o, "type", i.type.c_str());
		obs_data_set_int(o, "length", i.length);
		obs_data_set_string(o, "position", i.position.c_str());
		obs_data_set_string(o, "key", i.key.c_str());
		const char *status = "upcoming";
		if (cur_seq >= 0)
			status = i.sequence < cur_seq ? "done" : (i.sequence == cur_seq ? "current" : "upcoming");
		obs_data_set_string(o, "status", status);
		set_json_ts(o, "planned_start", planned);
		if (planned && i.type != "header")
			planned += i.length * 1000;
		obs_data_array_push_back(arr.p, o);
	}
	obs_data_set_array(root, "items", arr.p);
	return obs_data_get_json(root);
}

static ItemTime parse_item_time(obs_data_t *res)
{
	ItemTime t;
	OData a(obs_data_get_obj(res, "attributes"));
	t.id = str(res, "id");
	t.item_id = rel_id(res, "item");
	t.plan_time_id = rel_id(res, "plan_time");
	t.live_start_at = parse_ts(str(a, "live_start_at"));
	t.live_end_at = parse_ts(str(a, "live_end_at"));
	t.length = num(a, "length");
	return t;
}

static bool select_plan(FeedContext &c, Plan &out)
{
	const PcoConfig &cfg = c.cfg;
	std::string base = "/service_types/" + cfg.service_type + "/plans";
	if (!cfg.plan_id.empty()) {
		OData body = api_get(cfg, base + "/" + cfg.plan_id);
		OData d(obs_data_get_obj(body, "data"));
		if (!d)
			return false;
		out = parse_plan(d);
		return true;
	}
	bool have_past = false;
	Plan past;
	{
		OData body = api_get(cfg, base + "?filter=past&order=-sort_date&per_page=1");
		OArray data(obs_data_get_array(body, "data"));
		if (data.count()) {
			OData d = data.at(0);
			past = parse_plan(d);
			have_past = true;
			if (past.last_time_at && pco_now_ms() - past.last_time_at < PLAN_STICKY_MS) {
				out = past;
				return true;
			}
		}
	}
	OData body = api_get(cfg, base + "?filter=future&order=sort_date&per_page=1");
	OArray data(obs_data_get_array(body, "data"));
	if (data.count()) {
		OData d = data.at(0);
		out = parse_plan(d);
		return true;
	}
	if (have_past) {
		out = past;
		return true;
	}
	return false;
}

static std::string poll_once(FeedContext &c)
{
	const PcoConfig &cfg = c.cfg;
	double t = mono_s();

	if (!c.have_plan || t - c.plan_checked > PLAN_RECHECK_S) {
		Plan p;
		bool ok = select_plan(c, p);
		if (ok && (!c.have_plan || p.id != c.plan.id)) {
			obs_log(LOG_INFO, "Plan: %s (%s, ID %s)", p.title.c_str(), p.dates.c_str(), p.id.c_str());
			c.items_loaded = -1e9;
		}
		c.have_plan = ok;
		c.plan = p;
		c.plan_checked = t;
	}
	if (!c.have_plan)
		return build_state(nullptr, {}, {}, nullptr, nullptr);

	std::string base = "/service_types/" + cfg.service_type + "/plans/" + c.plan.id;

	if (t - c.items_loaded > ITEMS_RELOAD_S) {
		std::vector<Item> items;
		api_get_all(cfg, base + "/items?per_page=100", [&](obs_data_t *res) {
			OData a(obs_data_get_obj(res, "attributes"));
			Item i;
			i.id = str(res, "id");
			i.title = str(a, "title");
			i.type = str(a, "item_type");
			i.position = str(a, "service_position");
			i.key = str(a, "key_name");
			i.description = str(a, "description");
			i.length = num(a, "length");
			i.sequence = num(a, "sequence");
			items.push_back(i);
		});
		std::sort(items.begin(), items.end(),
			  [](const Item &a, const Item &b) { return a.sequence < b.sequence; });
		std::vector<PlanTime> times;
		api_get_all(cfg, base + "/plan_times?per_page=100", [&](obs_data_t *res) {
			OData a(obs_data_get_obj(res, "attributes"));
			PlanTime pt;
			pt.id = str(res, "id");
			pt.name = str(a, "name");
			pt.type = str(a, "time_type");
			pt.starts_at = parse_ts(str(a, "starts_at"));
			pt.ends_at = parse_ts(str(a, "ends_at"));
			pt.live_starts_at = parse_ts(str(a, "live_starts_at"));
			pt.live_ends_at = parse_ts(str(a, "live_ends_at"));
			times.push_back(pt);
		});
		c.items = std::move(items);
		c.plan_times = std::move(times);
		c.items_loaded = t;
	}

	bool have_cur = false, have_next = false;
	ItemTime cur, next;
	try {
		OData body = api_get(cfg, base + "/live?include=current_item_time,next_item_time");
		OData live(obs_data_get_obj(body, "data"));
		std::string cur_id = rel_id(live, "current_item_time");
		std::string next_id = rel_id(live, "next_item_time");
		OArray inc(obs_data_get_array(body, "included"));
		for (size_t i = 0; i < inc.count(); i++) {
			OData r = inc.at(i);
			if (str(r, "type") != "ItemTime")
				continue;
			std::string id = str(r, "id");
			if (!cur_id.empty() && id == cur_id) {
				cur = parse_item_time(r);
				have_cur = true;
			} else if (!next_id.empty() && id == next_id) {
				next = parse_item_time(r);
				have_next = true;
			}
		}
	} catch (const ApiError &e) {
		if (e.status != 404) // plan without LIVE session is not an error
			throw;
	}
	return build_state(&c.plan, c.items, c.plan_times, have_cur ? &cur : nullptr, have_next ? &next : nullptr);
}

/* demo data so the overlays can be designed without credentials */
static std::string demo_state(FeedContext &c)
{
	struct DemoItem {
		const char *title, *type;
		int length;
	};
	static const DemoItem demo[] = {
		{"Pre-Service", "header", 0},
		{"Countdown-Video", "media", 300},
		{"Begrüssung", "item", 180},
		{"Lobpreis", "header", 0},
		{"Way Maker", "song", 330},
		{"Build My Life", "song", 300},
		{"Gebet", "item", 120},
		{"Predigt: Hoffnung, die trägt", "item", 1800},
		{"Abschlusslied", "song", 240},
		{"Segen", "item", 60},
	};
	const int n_items = static_cast<int>(sizeof(demo) / sizeof(demo[0]));
	const long long pre_ms = 20000, cycle_ms = 90000;
	long long now = pco_now_ms();
	if (!c.demo_t0)
		c.demo_t0 = now;
	long long elapsed = now - c.demo_t0;

	Plan plan;
	plan.id = "demo";
	plan.title = "Demo-Plan";
	plan.series_title = "Herbstserie";
	plan.dates = "Demo";

	std::vector<Item> items;
	std::vector<int> playable;
	for (int i = 0; i < n_items; i++) {
		Item it;
		it.id = std::to_string(i);
		it.title = demo[i].title;
		it.type = demo[i].type;
		it.length = demo[i].length;
		it.sequence = i;
		it.key = it.type == "song" ? "G" : "";
		items.push_back(it);
		if (it.type != "header")
			playable.push_back(i);
	}
	PlanTime svc;
	svc.id = "1";
	svc.name = "Celebration";
	svc.type = "service";
	svc.starts_at = c.demo_t0 + pre_ms;
	svc.ends_at = svc.starts_at + 75 * 60000;
	std::vector<PlanTime> times{svc};

	if (elapsed < pre_ms)
		return build_state(&plan, items, times, nullptr, nullptr);

	long long k = ((elapsed - pre_ms) / cycle_ms) % static_cast<long long>(playable.size());
	ItemTime cur, next;
	cur.item_id = std::to_string(playable[static_cast<size_t>(k)]);
	cur.plan_time_id = "1";
	cur.length = 75;
	cur.live_start_at = now - (elapsed - pre_ms) % cycle_ms;
	next.item_id = std::to_string(playable[static_cast<size_t>((k + 1) % static_cast<long long>(playable.size()))]);
	times[0].live_starts_at = svc.starts_at;
	return build_state(&plan, items, times, &cur, &next);
}

static void feed_thread(std::shared_ptr<FeedContext> c)
{
	for (;;) {
		int interval;
		{
			std::lock_guard<std::mutex> lock(c->mtx);
			if (c->stop)
				break;
			interval = c->interval;
			if (c->reset_caches) {
				c->reset_caches = false;
				c->plan_checked = -1e9;
				c->items_loaded = -1e9;
			}
		}

		std::string state, status;
		bool rate_limited = false;
		const PcoConfig &cfg = c->cfg;
		if (cfg.demo) {
			state = demo_state(*c);
			status = "Demo";
			interval = 1;
		} else if (!cfg.has_credentials()) {
			state = "{\"ok\":false,\"error\":\"Application ID / Secret missing\"}";
			status = obs_module_text("Status.NoCredentials");
		} else if (cfg.service_type.empty()) {
			state = "{\"ok\":false,\"error\":\"No service type selected\"}";
			status = obs_module_text("Status.NoServiceType");
		} else {
			try {
				state = poll_once(*c);
				if (c->have_plan)
					status = std::string(obs_module_text("Status.OK")) + ": " + c->plan.title + " (" +
						 c->plan.dates + ")";
				else
					status = obs_module_text("Status.NoPlan");
			} catch (const ApiError &e) {
				rate_limited = e.status == 429;
				status = std::string(obs_module_text("Status.Error")) + ": " + e.message;
				obs_log(LOG_WARNING, "%s", e.message.c_str());
				std::lock_guard<std::mutex> lock(c->mtx);
				// keep the last good data, only flag the error
				OData old(obs_data_create_from_json(c->state.c_str()));
				if (old) {
					obs_data_set_bool(old, "ok", false);
					obs_data_set_string(old, "error", e.message.c_str());
					state = obs_data_get_json(old);
				}
			}
		}

		std::unique_lock<std::mutex> lock(c->mtx);
		if (!state.empty())
			c->state = state;
		c->status = status;
		int wait = rate_limited ? 20 : std::max(1, interval);
		c->cv.wait_for(lock, std::chrono::seconds(wait), [&] { return c->stop || c->force; });
		c->force = false;
		if (c->stop)
			break;
	}
	g_running_threads--;
}

/* ------------------------------------------------------------------------ */
/* registry                                                                  */
/* ------------------------------------------------------------------------ */

static std::mutex g_registry_mtx;
static std::map<std::string, std::weak_ptr<PcoFeed>> g_registry;

std::shared_ptr<PcoFeed> PcoFeed::acquire(const PcoConfig &cfg)
{
	std::lock_guard<std::mutex> lock(g_registry_mtx);
	std::string key = cfg.key();
	auto it = g_registry.find(key);
	if (it != g_registry.end()) {
		if (auto existing = it->second.lock())
			return existing;
	}
	std::shared_ptr<PcoFeed> feed(new PcoFeed(cfg));
	g_registry[key] = feed;
	return feed;
}

PcoFeed::PcoFeed(const PcoConfig &cfg) : ctx(std::make_shared<FeedContext>())
{
	ctx->cfg = cfg;
	g_running_threads++;
	std::thread(feed_thread, ctx).detach();
}

PcoFeed::~PcoFeed()
{
	{
		std::lock_guard<std::mutex> lock(ctx->mtx);
		ctx->stop = true;
	}
	ctx->cv.notify_all();
	// the thread is detached and finishes on its own (no blocking of the graphics thread)
}

std::string PcoFeed::state_json() const
{
	std::lock_guard<std::mutex> lock(ctx->mtx);
	return ctx->state;
}

std::string PcoFeed::status() const
{
	std::lock_guard<std::mutex> lock(ctx->mtx);
	return ctx->status;
}

void PcoFeed::set_interval(int seconds)
{
	std::lock_guard<std::mutex> lock(ctx->mtx);
	ctx->interval = std::max(1, std::min(30, seconds));
}

void PcoFeed::refresh()
{
	{
		std::lock_guard<std::mutex> lock(ctx->mtx);
		ctx->force = true;
		ctx->reset_caches = true;
	}
	ctx->cv.notify_all();
}

void pco_shutdown()
{
	// feeds are stopped by their owners; give running HTTP requests a moment to finish
	for (int i = 0; i < 100 && g_running_threads.load() > 0; i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
}
