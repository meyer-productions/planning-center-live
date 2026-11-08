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

struct HttpResponse {
	long status = 0;   // HTTP status code, 0 = no response
	std::string body;  // response body (UTF-8)
	std::string error; // transport error (empty if a response was received)
};

/* Blocking HTTPS GET. extra_headers: "Name: value\r\n" lines. */
HttpResponse http_get(const std::string &url, const std::string &extra_headers, int timeout_ms);
