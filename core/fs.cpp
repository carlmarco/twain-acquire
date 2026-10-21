/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "fs.hpp"

#include <chrono>
#include <cstdio>
#include <random>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace ta::fs {

namespace {

void setErr(std::string* err, const std::string& what) {
  if (err) *err = what;
}

std::string tempSuffix() {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  char buf[32];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(rng()));
  return buf;
}

#ifdef _WIN32
std::string lastError(const char* what) {
  return std::string(what) + " failed (Windows error " + std::to_string(GetLastError()) + ")";
}

bool writeAndFlush(HANDLE h, const void* data, size_t size, std::string* err) {
  const auto* p = static_cast<const uint8_t*>(data);
  while (size > 0) {
    DWORD chunk = size > 0x40000000 ? 0x40000000 : static_cast<DWORD>(size);
    DWORD wrote = 0;
    if (!WriteFile(h, p, chunk, &wrote, nullptr) || wrote == 0) {
      setErr(err, lastError("WriteFile"));
      return false;
    }
    p += wrote;
    size -= wrote;
  }
  if (!FlushFileBuffers(h)) {
    setErr(err, lastError("FlushFileBuffers"));
    return false;
  }
  return true;
}
#else
std::string lastError(const char* what) {
  return std::string(what) + " failed: " + std::strerror(errno);
}

bool writeAndFlush(int fd, const void* data, size_t size, std::string* err) {
  const auto* p = static_cast<const uint8_t*>(data);
  while (size > 0) {
    ssize_t n = ::write(fd, p, size);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      setErr(err, lastError("write"));
      return false;
    }
    p += n;
    size -= static_cast<size_t>(n);
  }
  if (::fsync(fd) != 0) {
    setErr(err, lastError("fsync"));
    return false;
  }
  return true;
}

void syncDir(const Path& dir) {
  int fd = ::open(dir.c_str(), O_RDONLY);
  if (fd >= 0) {
    ::fsync(fd);
    ::close(fd);
  }
}
#endif

}  // namespace

Path fromUtf8(std::string_view utf8) {
#if defined(__cpp_char8_t)
  return Path(std::u8string(utf8.begin(), utf8.end()));
#else
  return std::filesystem::u8path(utf8.begin(), utf8.end());
#endif
}

std::string toUtf8(const Path& p) {
  auto s = p.u8string();
  return std::string(s.begin(), s.end());
}

bool atomicWrite(const Path& dest, const void* data, size_t size, std::string* err) {
  const Path dir = dest.parent_path();
  const Path tmp = dir / fromUtf8("." + toUtf8(dest.filename()) + "." + tempSuffix() + ".tmp");
#ifdef _WIN32
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                         FILE_ATTRIBUTE_HIDDEN, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    setErr(err, lastError("CreateFileW"));
    return false;
  }
  bool ok = writeAndFlush(h, data, size, err);
  CloseHandle(h);
  if (ok && !MoveFileExW(tmp.c_str(), dest.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    setErr(err, lastError("MoveFileExW"));
    ok = false;
  }
  // A rename keeps the hidden attribute the temporary was created with; the
  // finished file must be visible, or programs watching the folder skip it.
  if (ok && !SetFileAttributesW(dest.c_str(), FILE_ATTRIBUTE_NORMAL)) {
    setErr(err, lastError("SetFileAttributesW"));
    ok = false;
  }
  if (!ok) DeleteFileW(tmp.c_str());
  return ok;
#else
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (fd < 0) {
    setErr(err, lastError("open"));
    return false;
  }
  bool ok = writeAndFlush(fd, data, size, err);
  ::close(fd);
  if (ok && ::rename(tmp.c_str(), dest.c_str()) != 0) {
    setErr(err, lastError("rename"));
    ok = false;
  }
  if (!ok) {
    ::unlink(tmp.c_str());
    return false;
  }
  syncDir(dir);
  return true;
#endif
}

bool appendDurable(const Path& file, std::string_view text, std::string* err) {
#ifdef _WIN32
  HANDLE h = CreateFileW(file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    setErr(err, lastError("CreateFileW"));
    return false;
  }
  bool ok = writeAndFlush(h, text.data(), text.size(), err);
  CloseHandle(h);
  return ok;
#else
  bool existed = std::filesystem::exists(file);
  int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) {
    setErr(err, lastError("open"));
    return false;
  }
  bool ok = writeAndFlush(fd, text.data(), text.size(), err);
  ::close(fd);
  if (ok && !existed) syncDir(file.parent_path());
  return ok;
#endif
}

bool readAll(const Path& file, std::vector<uint8_t>& out, std::string* err) {
#ifdef _WIN32
  FILE* f = _wfopen(file.c_str(), L"rb");
#else
  FILE* f = std::fopen(file.c_str(), "rb");
#endif
  if (!f) {
    setErr(err, "cannot open " + toUtf8(file));
    return false;
  }
  out.clear();
  uint8_t buf[1 << 16];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
  bool ok = !std::ferror(f);
  std::fclose(f);
  if (!ok) setErr(err, "read error on " + toUtf8(file));
  return ok;
}

bool readText(const Path& file, std::string& out, std::string* err) {
  std::vector<uint8_t> bytes;
  if (!readAll(file, bytes, err)) return false;
  out.assign(bytes.begin(), bytes.end());
  return true;
}

int64_t nowUnixMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace ta::fs
