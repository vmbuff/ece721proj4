// Project 4 - Value Prediction (competition branch)
//
// vpu_eves: EVES-style confidence-and-filtering layer on top of the SVP core.
// Structurally identical to `vpu` (same SVP table, same VPQ, same prediction
// formula, same repair semantics) but with four additions drawn from Seznec's
// CVP-1 winning EVES predictor (2018) and Perais & Seznec's HPCA 2014 paper:
//
//   1. Forward Probabilistic Counter (FPC) - conf increments with p<1 via an
//      internal 16-bit LFSR, so saturation requires ~128 correct predictions
//      rather than 31, meeting the ~99.5% break-even accuracy that VR-1
//      full-flush recovery demands (Perais & Seznec HPCA14, §III).
//
//   2. Per-instruction-type increment probabilities - single-cycle ALU ops
//      (the dominant source of confident-misses on hmmer/sjeng/bzip2) use
//      p=1/128, slow/FP ALU p=1/32, loads p=1/8. LLC-miss loads would be p=1
//      but we do not have cache-hint plumbing in this scope so they fold into
//      the L1-hit bucket. (Seznec CVP-1 §3.2.)
//
//   3. 128-instruction misprediction cooldown - after any value mispredict,
//      predict() returns no-hit for the next MISP_COOLDOWN retired insts.
//      Prevents the burst-of-flushes cascade observed at phase boundaries.
//      (Seznec CVP-1 "burst filter".)
//
//   4. SafeStride rate monitor - global 32-bit counters track stride-pred
//      accuracy; if misprediction rate exceeds 1/SAFESTRIDE_RATE_DENOM after
//      a warmup, stride predictions are suppressed until the next periodic
//      reset. (Seznec CVP-1.) This is the per-trace adaptive kill-switch.
//
// Explicitly NOT included (deferred to a Week-2 branch): VTAGE tagged banks,
// VR-5 selective squash, 2-delta stride, NotFirstOcc bit, hysteresis on miss,
// compact 106-bit entry layout.
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

    // --- EVES additions ---

    // Per-instruction-type FPC increment denominators, indexed by vp_inst_type.
    // p = 1/denom. Probability check is (lfsr_sample % denom) == 0 so any
    // positive integer is a valid denom (not only powers of 2).
    unsigned int p_incr_denom[VPT_COUNT];

    // 16-bit LFSR driving FPC probabilistic increment decisions
    uint16_t lfsr;

    // Misprediction cooldown counters
    uint64_t retire_count;                       // total retired VP-eligible instructions
    uint64_t last_misp_retire_count;             // retire_count at last value mispredict
    static constexpr unsigned MISP_COOLDOWN = 128;

    // SafeStride global-rate kill-switch counters
    uint32_t safestride_total;
    uint32_t safestride_miss;
    static constexpr uint32_t SAFESTRIDE_RATE_DENOM = 1024;   // suppress when miss/total > 1/1024
    static constexpr uint32_t SAFESTRIDE_WARMUP     = 4096;   // minimum total before gating
    static constexpr uint32_t SAFESTRIDE_RESET_PERIOD = 1000000;  // decay every ~1M retires

    // Private helpers (same logic as vpu)
    unsigned int get_svp_index(uint64_t pc);
    uint64_t     get_svp_tag(uint64_t pc);
    bool         tag_matches(unsigned int idx, uint64_t pc);
    uint64_t     count_inflight_instances(unsigned int svp_index);

    // 16-bit LFSR step; returns the new state.
    uint16_t     lfsr_step();

    // Returns true iff stride predictions are currently disabled by the
    // SafeStride rate monitor. Uses saturating comparison to avoid early
    // false positives during the warmup phase.
    bool         safestride_disabled() const;

public:
    // Constructor - base shape from --vp-eves=<VPQsize>,<indexbits>,<tagbits>,<confmax>;
    // per-type denominators come from optional --vp-eves-denoms (defaults 128/32/8).
    // No oracleconf parameter (oracle confidence bypasses EVES's whole point).
    vpu_eves(unsigned int vpq_size, unsigned int index_bits, unsigned int tag_bits, unsigned int conf_max,
             unsigned int denom_intalu, unsigned int denom_fpalu, unsigned int denom_load);
    ~vpu_eves() override;
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
