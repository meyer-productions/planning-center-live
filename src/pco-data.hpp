/*
Planning Center Live for OBS
Copyright (C) 2026 meyer.productions

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

struct PcoConfig {
	std::string app_id;
	std::string secret;
	std::string service_type;
	std::string plan_id; // optional override
	bool demo = false;

	std::string key() const;
	bool has_credentials() const { return !app_id.empty() && !secret.empty(); }
};

struct FeedContext;

/*
 * A feed polls Planning Center for one configuration in a background thread.
 * Sources with the same configuration share one feed (one set of API requests).
 */
class PcoFeed {
public:
	static std::shared_ptr<PcoFeed> acquire(const PcoConfig &cfg);
	~PcoFeed();

	/* Latest state as JSON (same structure as the overlay expects in "state"). */
	std::string state_json() const;
	/* Short human readable status for the properties dialog. */
	std::string status() const;

	void set_interval(int seconds);
	void refresh();

private:
	explicit PcoFeed(const PcoConfig &cfg);
	std::shared_ptr<FeedContext> ctx;
};

/* Blocking: lists the service types of the account (id, name). */
std::vector<std::pair<std::string, std::string>> pco_service_types(const std::string &app_id,
								    const std::string &secret, std::string &error);

/* Waits (briefly) for background threads on module unload. */
void pco_shutdown();

/* Milliseconds since the Unix epoch. */
long long pco_now_ms();
