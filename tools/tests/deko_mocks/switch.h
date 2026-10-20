// Minimal Horizon surface for the host-only presentation recorder.
#pragma once
#include <cstdint>
#include <stdexcept>
#define Module_Libnx 0
#define LibnxError_IoError 1
#define MAKERESULT(module, error) (error)
inline void *nwindowGetDefault() { return reinterpret_cast<void *>(1); }
[[noreturn]] inline void fatalThrow(uint32_t) { throw std::runtime_error("fatalThrow"); }
