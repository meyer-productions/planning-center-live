# Robust download of a build dependency (included inside the download loop of
# buildspec_common.cmake - ours and, patched, the one of the OBS sources).
#
# Expects: url, file, hash, dependencies_dir
# Sets:    download_status, error_code, error_message
#
# Old obs-deps release assets currently fail with HTTP 504 via the
# github.com/.../releases/download/... link, while the GitHub API asset
# endpoint works. Strategy: try the direct link, then fall back to the API
# (authenticated with GITHUB_TOKEN when available), up to 6 attempts.

set(error_code 1)
set(error_message "not attempted")
set(_pcl_auth)
if(DEFINED ENV{GITHUB_TOKEN} AND NOT "$ENV{GITHUB_TOKEN}" STREQUAL "")
  set(_pcl_auth HTTPHEADER "Authorization: Bearer $ENV{GITHUB_TOKEN}")
endif()

foreach(_pcl_attempt RANGE 1 6)
  set(_pcl_url "${url}")
  set(_pcl_headers)

  # source archives (github.com/<repo>/archive/refs/tags/<tag>.zip|.tar.gz): same file from codeload
  if(_pcl_attempt GREATER 1 AND url MATCHES "^https://github.com/([^/]+/[^/]+)/archive/(refs/tags/.+)\\.(zip|tar\\.gz)$")
    set(_pcl_url "https://codeload.github.com/${CMAKE_MATCH_1}/${CMAKE_MATCH_3}/${CMAKE_MATCH_2}")
  endif()

  if(_pcl_attempt GREATER 1 AND url MATCHES "^https://github.com/([^/]+/[^/]+)/releases/download/([^/]+)/(.+)$")
    set(_pcl_repo "${CMAKE_MATCH_1}")
    set(_pcl_tag "${CMAKE_MATCH_2}")
    set(_pcl_asset "${CMAKE_MATCH_3}")
    set(_pcl_json_file "${dependencies_dir}/.pcl-release-${_pcl_tag}.json")
    if(NOT EXISTS "${_pcl_json_file}")
      file(
        DOWNLOAD "https://api.github.com/repos/${_pcl_repo}/releases/tags/${_pcl_tag}" "${_pcl_json_file}"
        ${_pcl_auth}
        HTTPHEADER "Accept: application/vnd.github+json"
        STATUS _pcl_rel_status
      )
      list(GET _pcl_rel_status 0 _pcl_rel_code)
      if(NOT _pcl_rel_code EQUAL 0)
        file(REMOVE "${_pcl_json_file}")
      endif()
    endif()
    if(EXISTS "${_pcl_json_file}")
      file(READ "${_pcl_json_file}" _pcl_json)
      string(JSON _pcl_count ERROR_VARIABLE _pcl_err LENGTH "${_pcl_json}" assets)
      if(NOT _pcl_err AND _pcl_count GREATER 0)
        math(EXPR _pcl_last "${_pcl_count} - 1")
        foreach(_pcl_i RANGE ${_pcl_last})
          string(JSON _pcl_name GET "${_pcl_json}" assets ${_pcl_i} name)
          if(_pcl_name STREQUAL _pcl_asset)
            string(JSON _pcl_id GET "${_pcl_json}" assets ${_pcl_i} id)
            set(_pcl_url "https://api.github.com/repos/${_pcl_repo}/releases/assets/${_pcl_id}")
            set(_pcl_headers ${_pcl_auth} HTTPHEADER "Accept: application/octet-stream")
            break()
          endif()
        endforeach()
      endif()
    endif()
  endif()

  # no EXPECTED_HASH here: a failed download with EXPECTED_HASH raises a CMake error that
  # fails the whole configure step even if a later attempt succeeds -> verify separately
  file(
    DOWNLOAD "${_pcl_url}" "${dependencies_dir}/${file}"
    ${_pcl_headers}
    STATUS download_status
  )
  list(GET download_status 0 error_code)
  list(GET download_status 1 error_message)
  if(error_code EQUAL 0)
    file(SHA256 "${dependencies_dir}/${file}" _pcl_actual_hash)
    if(_pcl_actual_hash STREQUAL hash)
      break()
    endif()
    set(error_code 1)
    set(error_message "SHA256 mismatch (${_pcl_actual_hash})")
    set(download_status "1;${error_message}")
  endif()
  file(REMOVE "${dependencies_dir}/${file}")
  message(STATUS "Downloading ${url} - attempt ${_pcl_attempt} failed (${error_message}), retrying")
  if(_pcl_attempt LESS 6)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 10)
  endif()
endforeach()
