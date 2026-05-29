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

#ifndef BAREOS_STORED_BACKENDS_VIRTUAL_TAPE_DEVICE_H_
#define BAREOS_STORED_BACKENDS_VIRTUAL_TAPE_DEVICE_H_

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <sys/mtio.h>

#include "generic_tape_device.h"

namespace storagedaemon {

class virtual_tape_device : public generic_tape_device {
 public:
  bool setup() override;

  int d_open(const char* pathname, int flags, int mode) override;
  int d_close(int fd) override;
  int d_ioctl(int fd, ioctl_req_t request, char* op) override;
  ssize_t d_read(int fd, void* buffer, size_t count) override;
  ssize_t d_write(int fd, const void* buffer, size_t count) override;

 private:
  struct BlockInfo {
    std::string filename;
    size_t size;
  };

  using TapeFile = std::vector<BlockInfo>;

  std::filesystem::path root_dir_;
  std::vector<TapeFile> files_;
  size_t current_file_{0};
  size_t current_block_{0};
  uint64_t next_block_id_{0};
  std::chrono::milliseconds operation_delay_{0};
  std::thread::id last_writer_thread_{};

  bool LoadLayout();
  bool SaveLayout() const;
  bool ParseOptions();
  void EnsurePositionIsValid();
  void ResetPosition();
  void PositionAtEod();
  void AdvanceForWriterSwitch();
  void TruncateAtCurrentPosition();
  void MaybeDelay() const;
  std::filesystem::path MetadataPath() const;
  std::filesystem::path ControlPath() const;
  std::filesystem::path BlockPath(const std::string& filename) const;
  int HandleOperation(mtop& operation);
  void FillStatus(mtget& status) const;
};

} /* namespace storagedaemon */

#endif  // BAREOS_STORED_BACKENDS_VIRTUAL_TAPE_DEVICE_H_
