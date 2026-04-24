// Project 4 - Value Prediction (competition)
// Common interface implemented by both `vpu` (original SVP) and `vpu_eves`
// (EVES-style confidence and filtering layer). The pipeline holds a vpu_iface*
// so the same integration hooks work for either predictor.
#ifndef VPU_IFACE_H
#define VPU_IFACE_H

#include <cstdint>
#include <cstdio>

// Coarse classification passed by retire to train(). Used by vpu_eves to index
// per-instruction-type FPC increment probabilities (Seznec CVP-1 EVES table).
// The SVP `vpu` implementation ignores this parameter.
//
// LLCMISS is reserved for a future cache-hint-aware path (not plumbed in the
// Week-1 scope). Loads today default to L1HIT. The big lever is demoting
// SINGLE_CYCLE_ALU to p=1/128 so noisy 1-cycle integer ops never reach
// saturation on workloads like hmmer / sjeng / bzip2.
enum vp_inst_type : uint8_t {
    VPT_LOAD_LLCMISS     = 0,
    VPT_LOAD_L1HIT       = 1,
    VPT_SLOW_ALU_OR_FP   = 2,
    VPT_SINGLE_CYCLE_ALU = 3,
    VPT_COUNT            = 4
};

class vpu_iface {
public:
    virtual ~vpu_iface() = default;

    virtual bool         predict(uint64_t pc,
                                 uint64_t &out_predicted_val,
                                 bool &out_confident,
                                 unsigned int &out_vpq_index) = 0;

    virtual void         train(unsigned int vpq_index,
                               uint64_t committed_val,
                               uint8_t inst_type) = 0;

    virtual void         repair(unsigned int restored_vpq_tail,
                                bool restored_vpq_tail_phase) = 0;

    virtual unsigned int vpq_free_entries() = 0;

    virtual unsigned int get_vpq_tail() = 0;
    virtual bool         get_vpq_tail_phase() = 0;
    virtual unsigned int get_vpq_head() = 0;
    virtual bool         get_vpq_head_phase() = 0;

    virtual void         print_storage(FILE *out) = 0;
};

#endif // VPU_IFACE_H
