/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <deko3d.h>
#include <memory>
#include <string>

/* Borrows the renderer's device and command queue. The caller drains the
 * queue before destroying this object. All GPU resources are preallocated;
 * record() never compiles, allocates, waits or submits commands. */
class DrasticDekoLsfg {
public:
  static std::unique_ptr<DrasticDekoLsfg> create(DkDevice device,
      unsigned width, unsigned height, bool performance, unsigned flow_denominator,
      const char *dll_path, const char *cache_root, std::string &error);
  ~DrasticDekoLsfg();
  const DkImage *input() const;
  bool record(DkCmdBuf cmd, const DkImage *&generated,
              const DkImage *&original, std::string &error);
  void reset();
private:
  struct State;
  explicit DrasticDekoLsfg(std::unique_ptr<State> state);
  std::unique_ptr<State> state_;
};
