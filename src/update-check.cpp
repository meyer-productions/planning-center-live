/*
Planning Center Live for OBS
Copyright (C) 2026 meyer.productions

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "update-check.hpp"
#include "http-client.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif

#define RELEASES_API "https://api.github.com/repos/meyer-productions/planning-center-live/releases/latest"
#define RELEASES_PAGE "https://github.com/meyer-productions/planning-center-live/releases/latest"

static std::mutex g_mtx;
static UpdateInfo g_info;
static std::thread g_thread;
static std::atomic<bool> g_running{false};

/* "v1.2.3" / "1.2.3-beta" -> {1,2,3} */
static std::vector<int> parse_version(const std::string &v)
{
	std::vector<int> out;
	size_t i = (!v.empty() && (v[0] == 'v' || v[0] == 'V')) ? 1 : 0;
	while (i < v.size() && out.size() < 4) {
		int n = 0;
		bool digit = false;
		while (i < v.size() && v[i] >= '0' && v[i] <= '9') {
			n = n * 10 + (v[i] - '0');
			i++;
			digit = true;
		}
		if (!digit)
			break;
		out.push_back(n);
		if (i < v.size() && v[i] == '.')
			i++;
		else
			break;
	}
	while (out.size() < 3)
		out.push_back(0);
	return out;
}

static bool is_newer(const std::string &latest, const std::string &current)
{
	return parse_version(latest) > parse_version(current);
}

static bool ends_with(const std::string &s, const std::string &suffix)
{
	return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/* only open URLs that point to the project's GitHub downloads */
static bool safe_url(const std::string &url)
{
	const std::string prefix = "https://github.com/meyer-productions/planning-center-live/";
	if (url.compare(0, prefix.size(), prefix) != 0)
		return false;
	for (char c : url)
		if (!(isalnum(static_cast<unsigned char>(c)) || std::string(":/._-~%+").find(c) != std::string::npos))
			return false;
	return true;
}

static std::string platform_asset_suffix()
{
#if defined(_WIN32)
	return "-windows-x64.zip";
#elif defined(__APPLE__)
	return ".pkg";
#else
	return ""; // Linux: .deb or manual copy (Flatpak) -> release page
#endif
}

UpdateInfo pco_update_check_now()
{
	UpdateInfo info;
	info.checked = true;
	HttpResponse r = http_get(RELEASES_API, "Accept: application/vnd.github+json\r\n", 5000);
	if (!r.error.empty()) {
		info.error = r.error;
	} else if (r.status != 200) {
		info.error = "HTTP " + std::to_string(r.status);
	} else {
		obs_data_t *d = obs_data_create_from_json(r.body.c_str());
		if (!d) {
			info.error = "invalid response";
		} else {
			std::string tag = obs_data_get_string(d, "tag_name");
			info.latest = tag;
			if (!info.latest.empty() && (info.latest[0] == 'v' || info.latest[0] == 'V'))
				info.latest.erase(0, 1);
			info.url = obs_data_get_string(d, "html_url");
			std::string suffix = platform_asset_suffix();
			obs_data_array_t *assets = obs_data_get_array(d, "assets");
			size_t n = assets ? obs_data_array_count(assets) : 0;
			for (size_t i = 0; i < n && !suffix.empty(); i++) {
				obs_data_t *a = obs_data_array_item(assets, i);
				std::string name = obs_data_get_string(a, "name");
				if (ends_with(name, suffix))
					info.url = obs_data_get_string(a, "browser_download_url");
				obs_data_release(a);
			}
			obs_data_array_release(assets);
			obs_data_release(d);
			if (info.latest.empty())
				info.error = "no release found";
			else
				info.available = is_newer(info.latest, PLUGIN_VERSION);
			if (!safe_url(info.url))
				info.url = RELEASES_PAGE;
		}
	}
	if (!info.error.empty())
		obs_log(LOG_WARNING, "update check failed: %s", info.error.c_str());
	else if (info.available)
		obs_log(LOG_INFO, "update available: %s (installed %s) – %s", info.latest.c_str(), PLUGIN_VERSION,
			info.url.c_str());
	else
		obs_log(LOG_INFO, "up to date (%s)", PLUGIN_VERSION);

	std::lock_guard<std::mutex> lock(g_mtx);
	g_info = info;
	return info;
}

void pco_update_check_async()
{
	if (g_running.exchange(true))
		return;
	if (g_thread.joinable())
		g_thread.join();
	g_thread = std::thread([] {
		pco_update_check_now();
		g_running = false;
	});
}

UpdateInfo pco_update_info()
{
	std::lock_guard<std::mutex> lock(g_mtx);
	return g_info;
}

std::string pco_update_text()
{
	UpdateInfo i = pco_update_info();
	std::string s = std::string(obs_module_text("Update.Installed")) + " " + PLUGIN_VERSION + " – ";
	if (!i.checked)
		s += obs_module_text("Update.NotChecked");
	else if (!i.error.empty())
		s += std::string(obs_module_text("Update.Failed")) + " (" + i.error + ")";
	else if (i.available)
		s += std::string(obs_module_text("Update.Available")) + " " + i.latest;
	else
		s += obs_module_text("Update.UpToDate");
	return s;
}

void pco_update_open_download()
{
	UpdateInfo i = pco_update_info();
	std::string url = safe_url(i.url) ? i.url : RELEASES_PAGE;
#ifdef _WIN32
	ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
#ifdef __APPLE__
	const char *opener = "open";
#else
	const char *opener = "xdg-open"; // Flatpak: goes through the OpenURI portal
#endif
	char *argv[] = {const_cast<char *>(opener), const_cast<char *>(url.c_str()), nullptr};
	pid_t pid;
	if (posix_spawnp(&pid, opener, nullptr, nullptr, argv, environ) == 0) {
		std::thread([pid] {
			int st;
			waitpid(pid, &st, 0);
		}).detach();
	} else {
		obs_log(LOG_WARNING, "could not start %s for %s", opener, url.c_str());
	}
#endif
}

void pco_update_shutdown()
{
	if (g_thread.joinable())
		g_thread.join(); // at most the HTTP timeout (5 s)
}
