// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "iface.h"

namespace klear {

// Pass-through NS.
class NullNs : public INsBackend {
public:
    bool init(int sample_rate, std::string*) override {
        sample_rate_ = sample_rate;
        return true;
    }
    void process(int16_t*, std::size_t) override {}
    int algorithmic_delay_ms() const override { return 0; }
    void get_stats(NsStats* out) const override { *out = NsStats{}; }
    const char* name() const override { return "null"; }

private:
    int sample_rate_ = 0;
};

}  // namespace klear
