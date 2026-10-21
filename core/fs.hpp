/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ta::fs {

using Path = std::filesystem::path;

// Paths cross every API as UTF-8 so non-ASCII names survive on Windows.
Path fromUtf8(std::string_view utf8);
std::string toUtf8(const Path& p);

// Write `data` to a hidden temporary next to `dest`, flush it to disk, then
// rename it over `dest` in one step. A reader sees either the old file or the
// whole new one, never a partial file.
bool atomicWrite(const Path& dest, const void* data, size_t size, std::string* err = nullptr);

// Append one record and flush it to disk before returning.
bool appendDurable(const Path& file, std::string_view text, std::string* err = nullptr);

bool readAll(const Path& file, std::vector<uint8_t>& out, std::string* err = nullptr);
bool readText(const Path& file, std::string& out, std::string* err = nullptr);

int64_t nowUnixMs();

}  // namespace ta::fs
