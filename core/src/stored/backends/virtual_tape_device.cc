/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2026 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/

#include "include/bareos.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

#include "stored/backends/util.h"
#include "stored/sd_backends.h"
#include "virtual_tape_device.h"

namespace storagedaemon {

REGISTER_SD_BACKEND(virtual_tape, virtual_tape_device);

namespace {

constexpr const char* kMetadataFilename = "virtual-tape.meta";
constexpr const char* kControlFilename = "virtual-tape.control";
constexpr const char* kMetadataMagic = "BVTAPE1";
constexpr uint32_t kGstatEof = 0x80000000u;
constexpr uint32_t kGstatBot = 0x40000000u;
constexpr uint32_t kGstatEot = 0x20000000u;
constexpr uint32_t kGstatEod = 0x08000000u;
constexpr uint32_t kGstatOnline = 0x01000000u;
constexpr uint32_t kGstatImmediateReport = 0x00010000u;

bool ParseUint64(const std::string& value, uint64_t& parsed)
{
  try {
    size_t consumed = 0;
    parsed = std::stoull(value, &consumed, 10);
    return consumed == value.size();
  } catch (...) {
    return false;
  }
}

}  // namespace

bool virtual_tape_device::setup() { return ParseOptions(); }

bool virtual_tape_device::ParseOptions()
{
  operation_delay_ = std::chrono::milliseconds{0};
  if (!dev_options || dev_options[0] == '\0') { return true; }

  auto parsed_options = backends::util::parse_options(dev_options);
  if (auto* error = std::get_if<backends::util::error>(&parsed_options)) {
    Mmsg1(errmsg, T_("Unable to parse virtual_tape options: %s\n"),
          error->c_str());
    return false;
  }

  auto options = std::get<backends::util::options>(std::move(parsed_options));
  if (auto iter = options.find("delay"); iter != options.end()) {
    uint64_t parsed_delay = 0;
    if (!ParseUint64(iter->second, parsed_delay)) {
      Mmsg1(errmsg,
            T_("Invalid virtual_tape delay value \"%s\". Expected milliseconds.\n"),
            iter->second.c_str());
      return false;
    }

    operation_delay_ = std::chrono::milliseconds(parsed_delay);
    options.erase(iter);
  }

  if (!options.empty()) {
    std::string unknown;
    for (const auto& [key, value] : options) {
      if (!unknown.empty()) { unknown += ", "; }
      unknown += key;
      if (!value.empty()) { unknown += "=" + value; }
    }
    Mmsg1(errmsg, T_("Unknown virtual_tape device options: %s\n"),
          unknown.c_str());
    return false;
  }

  return true;
}

std::filesystem::path virtual_tape_device::MetadataPath() const
{
  return root_dir_ / kMetadataFilename;
}

std::filesystem::path virtual_tape_device::ControlPath() const
{
  return root_dir_ / kControlFilename;
}

std::filesystem::path virtual_tape_device::BlockPath(
    const std::string& filename) const
{
  return root_dir_ / filename;
}

void virtual_tape_device::EnsurePositionIsValid()
{
  if (files_.empty()) { files_.emplace_back(); }

  if (current_file_ >= files_.size()) { current_file_ = files_.size() - 1; }

  if (current_block_ > files_[current_file_].size()) {
    current_block_ = files_[current_file_].size();
  }
}

void virtual_tape_device::ResetPosition()
{
  current_file_ = 0;
  current_block_ = 0;
}

void virtual_tape_device::PositionAtEod()
{
  EnsurePositionIsValid();
  current_file_ = files_.size() - 1;
  current_block_ = files_.back().size();
}

void virtual_tape_device::AdvanceForWriterSwitch()
{
  EnsurePositionIsValid();

  const auto current_writer = std::this_thread::get_id();
  if (last_writer_thread_ == std::thread::id{}
      || last_writer_thread_ == current_writer
      || files_[current_file_].empty()) {
    last_writer_thread_ = current_writer;
    return;
  }

  if (current_block_ != files_[current_file_].size()) { TruncateAtCurrentPosition(); }

  files_.emplace_back();
  ++current_file_;
  current_block_ = 0;
  last_writer_thread_ = current_writer;
}

void virtual_tape_device::MaybeDelay() const
{
  if (operation_delay_.count() > 0) {
    std::this_thread::sleep_for(operation_delay_);
  }
}

bool virtual_tape_device::LoadLayout()
{
  files_.clear();
  files_.emplace_back();
  next_block_id_ = 0;
  ResetPosition();

  const auto metadata_path = MetadataPath();
  if (!std::filesystem::exists(metadata_path)) { return true; }

  std::ifstream input(metadata_path);
  if (!input.is_open()) {
    errno = EIO;
    return false;
  }

  std::string magic;
  if (!(input >> magic >> next_block_id_) || magic != kMetadataMagic) {
    errno = EINVAL;
    return false;
  }

  std::string record_type;
  while (input >> record_type) {
    if (record_type == "data") {
      BlockInfo block;
      if (!(input >> block.filename >> block.size)) {
        errno = EINVAL;
        return false;
      }
      files_.back().push_back(std::move(block));
    } else if (record_type == "filemark") {
      files_.emplace_back();
    } else {
      errno = EINVAL;
      return false;
    }
  }

  if (!input.eof()) {
    errno = EINVAL;
    return false;
  }

  EnsurePositionIsValid();
  return true;
}

bool virtual_tape_device::SaveLayout() const
{
  const auto metadata_path = MetadataPath();
  const auto temporary_path = metadata_path.string() + ".tmp";

  std::ofstream output(temporary_path, std::ios::trunc);
  if (!output.is_open()) {
    errno = EIO;
    return false;
  }

  output << kMetadataMagic << ' ' << next_block_id_ << '\n';
  for (size_t file_index = 0; file_index < files_.size(); ++file_index) {
    for (const auto& block : files_[file_index]) {
      output << "data " << block.filename << ' ' << block.size << '\n';
    }
    if (file_index + 1 < files_.size()) { output << "filemark\n"; }
  }

  output.close();
  if (!output) {
    errno = EIO;
    return false;
  }

  std::error_code ec;
  std::filesystem::rename(temporary_path, metadata_path, ec);
  if (ec) {
    errno = EIO;
    return false;
  }

  return true;
}

void virtual_tape_device::TruncateAtCurrentPosition()
{
  EnsurePositionIsValid();

  for (size_t file_index = current_file_; file_index < files_.size();
       ++file_index) {
    size_t first_block = (file_index == current_file_) ? current_block_ : 0;
    for (size_t block_index = first_block; block_index < files_[file_index].size();
         ++block_index) {
      std::error_code ec;
      std::filesystem::remove(
          BlockPath(files_[file_index][block_index].filename), ec);
    }
  }

  files_[current_file_].resize(current_block_);
  files_.resize(current_file_ + 1);
  if (files_.empty()) { files_.emplace_back(); }
}

int virtual_tape_device::d_open(const char* pathname, int, int)
{
  root_dir_ = pathname;
  std::error_code ec;
  std::filesystem::create_directories(root_dir_, ec);
  if (ec) {
    errno = EIO;
    return -1;
  }

  if (!LoadLayout()) { return -1; }

  return ::open(ControlPath().c_str(), O_CREAT | O_RDWR, 0600);
}

int virtual_tape_device::d_close(int descriptor)
{
  if (descriptor >= 0) { return ::close(descriptor); }
  return 0;
}

void virtual_tape_device::FillStatus(mtget& status) const
{
  memset(&status, 0, sizeof(status));
  status.mt_fileno = current_file_;
  status.mt_blkno = current_block_;
  status.mt_gstat = kGstatOnline | kGstatImmediateReport;

  if (current_file_ == 0 && current_block_ == 0) { status.mt_gstat |= kGstatBot; }

  const bool at_end_of_file = current_block_ == files_[current_file_].size();
  const bool at_end_of_tape
      = current_file_ + 1 == files_.size() && at_end_of_file;

  if (at_end_of_file) { status.mt_gstat |= kGstatEof; }
  if (at_end_of_tape) { status.mt_gstat |= kGstatEot | kGstatEod; }
}

int virtual_tape_device::HandleOperation(mtop& operation)
{
  switch (operation.mt_op) {
    case MTREW:
    case MTOFFL:
    case MTLOAD:
      ResetPosition();
      return 0;
    case MTSETBLK:
    case MTSETDRVBUFFER:
    case MTLOCK:
    case MTUNLOCK:
      return 0;
    case MTEOM:
      PositionAtEod();
      MaybeDelay();
      return 0;
    case MTWEOF:
      TruncateAtCurrentPosition();
      for (int count = 0; count < operation.mt_count; ++count) {
        files_.emplace_back();
        ++current_file_;
        current_block_ = 0;
      }
      last_writer_thread_ = {};
      MaybeDelay();
      return SaveLayout() ? 0 : -1;
    case MTFSF:
      if (operation.mt_count < 0) {
        errno = EINVAL;
        return -1;
      }
      if (current_file_ + static_cast<size_t>(operation.mt_count) < files_.size()) {
        current_file_ += operation.mt_count;
        current_block_ = 0;
        return 0;
      }
      PositionAtEod();
      errno = EIO;
      return -1;
    case MTBSF:
      if (operation.mt_count < 0) {
        errno = EINVAL;
        return -1;
      }
      if (current_file_ >= static_cast<size_t>(operation.mt_count)) {
        current_file_ -= operation.mt_count;
        current_block_ = 0;
        return 0;
      }
      ResetPosition();
      errno = EIO;
      return -1;
    case MTFSR: {
      if (operation.mt_count < 0) {
        errno = EINVAL;
        return -1;
      }
      const auto new_block = current_block_ + operation.mt_count;
      if (new_block <= files_[current_file_].size()) {
        current_block_ = new_block;
        return 0;
      }
      current_block_ = files_[current_file_].size();
      errno = EIO;
      return -1;
    }
    case MTBSR:
      if (operation.mt_count < 0) {
        errno = EINVAL;
        return -1;
      }
      if (current_block_ >= static_cast<size_t>(operation.mt_count)) {
        current_block_ -= operation.mt_count;
        return 0;
      }
      current_block_ = 0;
      errno = EIO;
      return -1;
    default:
      errno = ENOTTY;
      return -1;
  }
}

int virtual_tape_device::d_ioctl(int, ioctl_req_t request, char* op)
{
  EnsurePositionIsValid();

  if (request == static_cast<ioctl_req_t>(MTIOCGET)) {
    auto* status = reinterpret_cast<mtget*>(op);
    FillStatus(*status);
    return 0;
  }

  if (request != static_cast<ioctl_req_t>(MTIOCTOP) || op == nullptr) {
    errno = ENOTTY;
    return -1;
  }

  auto* operation = reinterpret_cast<mtop*>(op);
  return HandleOperation(*operation);
}

ssize_t virtual_tape_device::d_read(int, void* buffer, size_t count)
{
  EnsurePositionIsValid();

  if (current_block_ >= files_[current_file_].size()) {
    if (current_file_ + 1 < files_.size()) {
      ++current_file_;
      current_block_ = 0;
    }
    return 0;
  }

  const auto& block = files_[current_file_][current_block_];
  if (count < block.size) {
    errno = ENOMEM;
    return -1;
  }

  std::ifstream input(BlockPath(block.filename), std::ios::binary);
  if (!input.is_open()) {
    errno = EIO;
    return -1;
  }

  input.read(reinterpret_cast<char*>(buffer), block.size);
  if (static_cast<size_t>(input.gcount()) != block.size || !input.good()) {
    if (!input.eof()) {
      errno = EIO;
      return -1;
    }
  }

  ++current_block_;
  return block.size;
}

ssize_t virtual_tape_device::d_write(int, const void* buffer, size_t count)
{
  EnsurePositionIsValid();
  AdvanceForWriterSwitch();
  TruncateAtCurrentPosition();

  char block_name[64];
  Bsnprintf(block_name, sizeof(block_name), "block-%020llu.bin",
            static_cast<unsigned long long>(next_block_id_++));
  const std::filesystem::path block_path = BlockPath(block_name);

  std::ofstream output(block_path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    errno = EIO;
    return -1;
  }

  output.write(reinterpret_cast<const char*>(buffer), count);
  output.close();
  if (!output) {
    errno = EIO;
    return -1;
  }

  files_[current_file_].push_back({block_name, count});
  ++current_block_;
  MaybeDelay();

  if (!SaveLayout()) { return -1; }

  return count;
}

} /* namespace storagedaemon */
