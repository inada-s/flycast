# SPDX-FileCopyrightText: 2026 gdxsv contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# gdxsv version scheme, included right after the top-level git describe and before core/version.h is generated.
# GIT_VERSION comes from the latest gdxsv-* tag, in semver form.
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
	if(MINGW AND DEFINED ENV{MSYSTEM} AND DEFINED ENV{GITHUB_ACTIONS})
		set(GDXSV_PATTERN "gdxsv-\\*")
	else()
		set(GDXSV_PATTERN "gdxsv-*")
	endif()
	message(GDXSV_PATTERN="${GDXSV_PATTERN}")
	execute_process(
		COMMAND ${GIT_EXECUTABLE} describe --tags --always --match ${GDXSV_PATTERN}
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		OUTPUT_VARIABLE GIT_VERSION
		OUTPUT_STRIP_TRAILING_WHITESPACE
	)
	# Transform git describe to semver format, e.g. gdxsv-1.2.4-3-g89b14873e -> gdxsv-1.2.4-dev.3+89b14873e
	string(REGEX REPLACE "-([0-9]+)-g(.+)$" "-dev.\\1+\\2" GIT_VERSION "${GIT_VERSION}")

	# Auto increment semver if it is prelease, since 1.2.3-beta.1 < 1.2.3
	string(REGEX MATCH "[0-9]+-dev" SEMVER_PATCH "${GIT_VERSION}")
	if(DEFINED SEMVER_PATCH)
		string(REPLACE "-dev" "" SEMVER_PATCH "${SEMVER_PATCH}")
		MATH(EXPR SEMVER_PATCH "${SEMVER_PATCH}+1")
		string(REGEX REPLACE "[0-9]+-dev" "${SEMVER_PATCH}-dev" GIT_VERSION "${GIT_VERSION}")
	endif()
endif()

message(GIT_VERSION="${GIT_VERSION}")
message(GIT_HASH="${GIT_HASH}")
