#pragma once
#include "model.hpp"
#include <memory>

namespace dtm {
// Call from a single worker thread. This class never creates a timer/thread.
// A nonempty onlyIds list polls only these drives; other cached records are kept.
class Sensors {
public:
    Sensors();
    ~Sensors();
    Sensors(const Sensors&) = delete;
    Sensors& operator=(const Sensors&) = delete;
    std::vector<Drive> poll(bool rediscover = false,
                          const std::vector<std::wstring>& onlyIds = {});
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
