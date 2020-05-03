// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "internal/file.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#include "absl/base/attributes.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "absl/utility/utility.h"
#pragma GCC diagnostic pop

#include "internal/int.h"
#include "internal/random.h"
#include "internal/str.h"

namespace phst_rules_elisp {

StatusOr<File> File::Open(std::string path, const FileMode mode) {
  File result;
  RETURN_IF_ERROR(result.DoOpen(std::move(path), mode));
  return std::move(result);
}

File::File() = default;

File::File(File&& other)
    : fd_(absl::exchange(other.fd_, -1)), path_(std::move(other.path_)) {}

File& File::operator=(File&& other) {
  const auto status = this->Close();
  if (!status.ok()) std::clog << status << std::endl;
  fd_ = absl::exchange(other.fd_, -1);
  path_ = std::move(other.path_);
  return *this;
}

File::~File() noexcept {
  if (fd_ < 0) return;
  std::clog << "file " << path_ << " still open" << std::endl;
  const auto status = this->Close();
  if (!status.ok()) std::clog << "error closing file: " << status << std::endl;
}

absl::Status File::DoOpen(std::string path, const FileMode mode) {
  int fd = -1;
  while (true) {
    fd = ::open(Pointer(path), static_cast<int>(mode) | O_CLOEXEC,
                S_IRUSR | S_IWUSR);
    if (fd >= 0 || errno != EINTR) break;
  }
  if (fd < 0) return ErrnoStatus("open", path);
  fd_ = fd;
  path_ = std::move(path);
  return absl::OkStatus();
}

StatusOr<std::FILE*> File::OpenCFile(const char* const mode) {
  const auto file = fdopen(fd_, mode);
  if (file == nullptr) return this->Fail("fdopen");
  return file;
}

absl::Status File::Close() {
  if (fd_ < 0) return absl::OkStatus();
  int status = -1;
  while (true) {
    status = ::close(fd_);
    if (status == 0 || errno != EINTR) break;
  }
  fd_ = -1;
  if (status != 0) return this->Fail("close");
  const auto close_status = this->DoClose();
  path_.clear();
  return close_status;
}

absl::Status File::DoClose() { return absl::OkStatus(); }

absl::Status File::Write(const absl::string_view data) {
  static_assert(
      UnsignedMax<ssize_t>() <= UnsignedMax<absl::string_view::size_type>(),
      "unsupported architecture");
  absl::string_view::size_type written = 0;
  auto rest = data;
  while (!rest.empty()) {
    const auto m = ::write(fd_, rest.data(), rest.size());
    if (m < 0) return this->Fail("write");
    if (m == 0) break;
    const auto n = static_cast<absl::string_view::size_type>(m);
    written += n;
    rest.remove_prefix(n);
  }
  assert(written <= data.size());
  if (written != data.size()) {
    return absl::DataLossError(absl::StrCat("requested write of ", data.size(),
                                            " bytes, but only ", written,
                                            " bytes written"));
  }
  return absl::OkStatus();
}

StatusOr<std::string> File::Read() {
  static_assert(UnsignedMax<ssize_t>() <= UnsignedMax<std::size_t>(),
                "unsupported architecture");
  std::array<char, 0x1000> buffer;
  std::string result;
  while (true) {
    const auto m = ::read(fd_, buffer.data(), buffer.size());
    if (m < 0) return this->Fail("read");
    if (m == 0) break;
    const auto n = static_cast<std::string::size_type>(m);
    result.append(buffer.data(), n);
  }
  return result;
}

absl::Status File::Fail(const absl::string_view function) const {
  const auto status = ErrnoStatus(function);
  return absl::Status(status.code(),
                      absl::StrCat("file ", path_, ": ", status.message()));
}

StatusOr<TempFile> TempFile::Create(const std::string& directory,
                                    const absl::string_view tmpl,
                                    Random& random) {
  TempFile result;
  for (int i = 0; i < 10; i++) {
    auto name = JoinPath(directory, random.TempName(tmpl));
    if (!FileExists(name)) {
      RETURN_IF_ERROR(result.DoOpen(std::move(name), FileMode::kReadWrite |
                                                         FileMode::kCreate |
                                                         FileMode::kExclusive));
      return std::move(result);
    }
  }
  return absl::UnavailableError(
      absl::StrCat("can’t create temporary file in directory ", directory,
                   " with template ", tmpl));
}

TempFile::TempFile() {}

TempFile::~TempFile() noexcept {
  const auto& path = this->path();
  const auto status = this->Remove();
  // Only print an error if removing the file failed (“false” return value), but
  // the file wasn’t already removed before (zero error code).
  if (!status.ok() && !absl::IsNotFound(status)) {
    std::clog << "error removing temporary file " << path << ": " << status
              << std::endl;
  }
}

absl::Status TempFile::DoClose() {
  return this->Remove();
}

absl::Status TempFile::Remove() noexcept {
  return RemoveFile(this->path());
}

std::string JoinPathImpl(const std::initializer_list<absl::string_view> pieces) {
  assert(pieces.begin() < pieces.end());
  const auto format = [](std::string* const out, absl::string_view name) {
    absl::ConsumePrefix(&name, "/");
    absl::ConsumeSuffix(&name, "/");
    out->append(name.begin(), name.end());
  };
  // Make sure to not strip leading or trailing slashes.
  return absl::StrCat(absl::StartsWith(*pieces.begin(), "/") ? "/" : "",
                      absl::StrJoin(pieces, "/", format),
                      absl::EndsWith(*(pieces.end() - 1), "/") ? "/" : "");
}

StatusOr<std::string> MakeAbsolute(const absl::string_view name) {
  if (IsAbsolute(name)) return std::string(name);
  struct free {
    void operator()(void* const ptr) const noexcept { std::free(ptr); }
  };
  const std::unique_ptr<char, free> dir(get_current_dir_name());
  if (dir == nullptr) return ErrnoStatus("get_current_dir_name");
  return JoinPath(dir.get(), name);
}

ABSL_MUST_USE_RESULT bool FileExists(const std::string& name) noexcept {
  struct stat info;
  return ::lstat(Pointer(name), &info) == 0;
}

absl::Status RemoveFile(const std::string& name) noexcept {
  return ::unlink(Pointer(name)) == 0 ? absl::OkStatus()
                                      : ErrnoStatus("unlink", name);
}

ABSL_MUST_USE_RESULT std::string TempDir() {
  const std::array<const char*, 2> vars = {"TEST_TMPDIR", "TMPDIR"};
  for (const auto var : vars) {
    const auto value = std::getenv(var);
    if (value != nullptr && *value != '\0') return value;
  }
  return "/tmp";
}

StatusOr<Directory> Directory::Open(const std::string& name) {
  const auto dir = ::opendir(Pointer(name));
  if (dir == nullptr) return ErrnoStatus("opendir", name);
  return Directory(dir);
}

Directory& Directory::operator=(Directory&& other) {
  const auto status = this->Close();
  if (!status.ok()) std::clog << status << std::endl;
  dir_ = absl::exchange(other.dir_, nullptr);
  return *this;
}

Directory::~Directory() noexcept {
  const auto status = this->Close();
  if (!status.ok()) std::clog << status << std::endl;
}

absl::Status Directory::Close() noexcept {
  if (dir_ == nullptr) return absl::OkStatus();
  const auto status =
      ::closedir(dir_) == 0 ? absl::OkStatus() : ErrnoStatus("closedir");
  dir_ = nullptr;
  return status;
}

void Directory::Iterator::Advance() {
  errno = 0;
  const auto entry = ::readdir(dir_);
  if (entry == nullptr) {
    dir_ = nullptr;
    if (errno != 0) std::clog << ErrnoStatus("readdir") << std::endl;
    entry_.clear();
  } else {
    entry_ = entry->d_name;
  }
}

}  // phst_rules_elisp
