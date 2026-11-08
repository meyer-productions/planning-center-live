/*
Planning Center Live for OBS
Copyright (C) 2026 meyer.productions

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "http-client.hpp"

#include <vector>

static const char *USER_AGENT = "planning-center-live-obs";

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

static std::wstring widen(const std::string &s)
{
	if (s.empty())
		return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
	std::wstring w(static_cast<size_t>(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
	return w;
}

struct WinHttpHandle {
	HINTERNET h = nullptr;
	explicit WinHttpHandle(HINTERNET handle) : h(handle) {}
	~WinHttpHandle()
	{
		if (h)
			WinHttpCloseHandle(h);
	}
	WinHttpHandle(const WinHttpHandle &) = delete;
	WinHttpHandle &operator=(const WinHttpHandle &) = delete;
};

static std::string last_error(const char *what)
{
	return std::string(what) + " failed (WinHTTP error " + std::to_string(GetLastError()) + ")";
}

HttpResponse http_get(const std::string &url, const std::string &extra_headers, int timeout_ms)
{
	HttpResponse r;
	std::wstring wurl = widen(url);

	URL_COMPONENTS uc = {};
	uc.dwStructSize = sizeof(uc);
	uc.dwSchemeLength = (DWORD)-1;
	uc.dwHostNameLength = (DWORD)-1;
	uc.dwUrlPathLength = (DWORD)-1;
	uc.dwExtraInfoLength = (DWORD)-1;
	if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
		r.error = "invalid URL";
		return r;
	}
	std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
	std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
	if (uc.lpszExtraInfo && uc.dwExtraInfoLength)
		path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);

	WinHttpHandle session(WinHttpOpen(widen(USER_AGENT).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
					  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
	if (!session.h) {
		r.error = last_error("WinHttpOpen");
		return r;
	}
	WinHttpSetTimeouts(session.h, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

	WinHttpHandle connect(WinHttpConnect(session.h, host.c_str(), uc.nPort, 0));
	if (!connect.h) {
		r.error = last_error("WinHttpConnect");
		return r;
	}

	DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
	WinHttpHandle request(WinHttpOpenRequest(connect.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
						 WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
	if (!request.h) {
		r.error = last_error("WinHttpOpenRequest");
		return r;
	}

	std::wstring headers = widen(extra_headers);
	if (!WinHttpSendRequest(request.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
				headers.empty() ? 0 : (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
		r.error = last_error("WinHttpSendRequest");
		return r;
	}
	if (!WinHttpReceiveResponse(request.h, nullptr)) {
		r.error = last_error("WinHttpReceiveResponse");
		return r;
	}

	DWORD status = 0;
	DWORD size = sizeof(status);
	WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			    WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
	r.status = static_cast<long>(status);

	for (;;) {
		DWORD avail = 0;
		if (!WinHttpQueryDataAvailable(request.h, &avail)) {
			r.error = last_error("WinHttpQueryDataAvailable");
			break;
		}
		if (avail == 0)
			break;
		std::vector<char> buf(avail);
		DWORD read = 0;
		if (!WinHttpReadData(request.h, buf.data(), avail, &read)) {
			r.error = last_error("WinHttpReadData");
			break;
		}
		r.body.append(buf.data(), read);
	}
	return r;
}

#else /* libcurl */

#include <curl/curl.h>
#include <mutex>

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	static_cast<std::string *>(userdata)->append(ptr, size * nmemb);
	return size * nmemb;
}

HttpResponse http_get(const std::string &url, const std::string &extra_headers, int timeout_ms)
{
	static std::once_flag init_flag;
	std::call_once(init_flag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

	HttpResponse r;
	CURL *curl = curl_easy_init();
	if (!curl) {
		r.error = "curl_easy_init failed";
		return r;
	}

	struct curl_slist *list = nullptr;
	size_t start = 0;
	while (start < extra_headers.size()) {
		size_t end = extra_headers.find("\r\n", start);
		if (end == std::string::npos)
			end = extra_headers.size();
		if (end > start)
			list = curl_slist_append(list, extra_headers.substr(start, end - start).c_str());
		start = end + 2;
	}

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.body);

	CURLcode rc = curl_easy_perform(curl);
	if (rc != CURLE_OK) {
		r.error = curl_easy_strerror(rc);
	} else {
		long code = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
		r.status = code;
	}
	curl_slist_free_all(list);
	curl_easy_cleanup(curl);
	return r;
}

#endif
