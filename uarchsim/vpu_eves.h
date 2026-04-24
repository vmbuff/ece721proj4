// Project 4 - Value Prediction (competition)
// EVES variant of the SVP. Same table + VPQ + prediction formula, but
// with four additions on top:
//   1. probabilistic confidence counter (only increments with p<1)
//   2. different probabilities for INT, FP, and LOAD
//   3. cooldown window after a mispredict
//   4. a global kill-switch that turns off stride predictions if the
//      misprediction rate gets too high
// See Perais & Seznec HPCA 2014 and Seznec's CVP-1 submission.
#ifndef VPU_EVES_H
#define VPU_EVES_H

#include <cstdint>
#include <cstdio>

#include "vpu_iface.h"

class vpu_eves : public vpu_iface {
private:
    struct svp_entry_t {
        uint64_t     tag;
        int64_t      stride;
        uint64_t     retired_value;
        uint64_t     instance;
        unsigned int conf;
        bool         valid;
    };

    struct vpq_entry_t {
        uint64_t     pc;
        unsigned int svp_index;
        uint64_t     predicted_value;
        bool         svp_hit;
        bool         confident;
    };

    // SVP table (direct-mapped, same as baseline)
    svp_entry_t *svp;
    unsigned int svp_num_entries;
    unsigned int svp_index_bits;
    unsigned int svp_tag_bits;
    unsigned int svp_conf_max;

    // VPQ circular buffer
    vpq_entry_t *vpq;
    unsigned int vpq_size;
    unsigned int vpq_head;
    bool         vpq_head_phase;
    unsigned int vpq_tail;
    bool         vpq_tail_phase;

    // EVES-specific state

    // Per-instruction-type increment denominators. p = 1/denom.
    // The check is (lfsr_sample % denom) == 0 so any positive denom works.
    unsigned int p_incr_denom[VPT_COUNT];

    // 16-bit LFSR used to generate the probabilistic samples
    uint16_t lfsr;

    // Cooldown counters
    uint64_t retire_count;              // total retired VP-eligible instructions
    uint64_t last_misp_retire_count;    // retire_count at last value mispredict
    static const unsigned int MISP_COOLDOWN = 128;

    // SafeStride counters (global kill-switch)
    uint32_t safestride_total;
    uint32_t safestride_miss;
    static const uint32_t SAFESTRIDE_RATE_DENOM = 1024;     // off if miss/total > 1/1024
    static const uint32_t SAFESTRIDE_WARMUP     = 4096;     // min total before we use it
    static const uint32_t SAFESTRIDE_RESET_PERIOD = 1000000; // halve counters every ~1M retires

    // Private helpers (same logic as vpu)
    unsigned int get_svp_index(uint64_t pc);
    uint64_t     get_svp_tag(uint64_t pc);
    bool         tag_matches(unsigned int idx, uint64_t pc);
    uint64_t     count_inflight_instances(unsigned int svp_index);

    // Steps the LFSR and returns the new 16-bit value.
    uint16_t     lfsr_step();

    // Returns true if the stride predictor is currently turned off because
    // too many mispredicts happened recently (SafeStride kill-switch).
    bool         safestride_disabled();

public:
    // Constructor - base config from --vp-eves=<VPQsize>,<indexbits>,<tagbits>,<confmax>
    // and per-type denominators from --vp-eves-denoms (defaults 128/32/8).
    vpu_eves(unsigned int vpq_size, unsigned int index_bits, unsigned int tag_bits, unsigned int conf_max,
             unsigned int denom_intalu, unsigned int denom_fpalu, unsigned int denom_load);
    ~vpu_eves();
    vpu_eves(const vpu_eves&) = delete;
    vpu_eves& operator=(const vpu_eves&) = delete;

    bool predict(uint64_t pc, uint64_t &out_predicted_val, bool &out_confident, unsigned int &out_vpq_index) override;
    void train(unsigned int vpq_index, uint64_t committed_val, uint8_t inst_type) override;
    void repair(unsigned int restored_vpq_tail, bool restored_vpq_tail_phase) override;

    unsigned int vpq_free_entries() override;

    unsigned int get_vpq_tail() override;
    bool         get_vpq_tail_phase() override;
    unsigned int get_vpq_head() override;
    bool         get_vpq_head_phase() override;

    void print_storage(FILE *out) override;
};

#endif // VPU_EVES_H
