#pragma once

#include <memory>

namespace pv {

// Saturation done by the GPU display pipeline (NVIDIA Digital Vibrance / AMD saturation).
// Zero render cost, no DWM involvement, works in exclusive fullscreen too.
// Both are loaded dynamically from the driver DLLs; no SDK needed.
class DriverSaturation {
public:
    DriverSaturation();
    ~DriverSaturation();
    DriverSaturation(const DriverSaturation&) = delete;
    DriverSaturation& operator=(const DriverSaturation&) = delete;

    bool Available() const noexcept;
    bool SupportsDesaturate() const noexcept;  // AMD: yes, NVIDIA: only >= 1.0
    const char* Name() const noexcept;

    bool Set(float saturation) noexcept;  // 1.0 = driver default
    void Restore() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

}  // namespace pv
