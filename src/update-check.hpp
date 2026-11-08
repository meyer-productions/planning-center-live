/*
Planning Center Live for OBS
Copyright (C) 2026 meyer.productions

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <string>

/*
 * Update check against the latest GitHub release. Nothing is installed automatically:
 * the user gets a hint and a button that opens the matching download in the browser.
 */
struct UpdateInfo {
	bool checked = false;   // a check has finished (successfully or not)
	bool available = false; // newer release than PLUGIN_VERSION
	std::string latest;     // e.g. "1.1.0"
	std::string url;        // download for this platform (fallback: release page)
	std::string error;      // last error, empty if OK
};

/* Starts a check in the background (no-op if one is running). */
void pco_update_check_async();
/* Blocking check (used by the "check for updates" button). */
UpdateInfo pco_update_check_now();
/* Last known result. */
UpdateInfo pco_update_info();
/* One line for the properties dialog. */
std::string pco_update_text();
/* Opens the download URL of the last check in the default browser. */
void pco_update_open_download();
/* Waits for a running background check (module unload). */
void pco_update_shutdown();
