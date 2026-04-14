// vpu.h
// Stride Value Predictor (SVP) + Value Prediction Queue (VPQ)
//
// SVP: direct-mapped table indexed by PC, learns stride patterns per PC.
// VPQ: circular FIFO tracking in-flight VP-eligible instrs. Needed because
//      SVP trains in-order at retire, but instrs execute OOO. Also handles
//      instance counter init on replacement and repair on squash.
// VPQ is a structural hazard, rename stalls if not enough free entries.
// Only SVP counts toward competition storage budget (VPQ excluded).

#ifndef VPU_H
#define VPU_H

#include <cstdint>
#include <cstdio>

class vpu_t {

private:

    // SVP entry
    // prediction = retired_value + instance * stride
    // instance: speculatively incremented at rename, decremented at retire,
    //           repaired on squash. Tracks how many in-flight copies of this PC exist.
    // conf: saturating counter, prediction only injected when conf == conf_max.
    //       increments when stride confirmed at retire, resets when stride changes.
    struct svp_entry_t {
        uint64_t     tag;
        int64_t      stride;         // must be signed per spec
        uint64_t     retired_value;  // last committed value
        uint64_t     instance;       // in-flight count (speculative)
        unsigned int conf;
        bool         valid;
    };

    // VPQ entry
    // Allocated at rename (tail), freed at retire (head).
    // Stores enough to train SVP at retire and compute vpmeas_* stats.
    // predicted_value stored even for unconfident predictions (needed for stats).
    struct vpq_entry_t {
        uint64_t     pc;
        unsigned int svp_index;       // cached so we don't recompute at retire
        uint64_t     predicted_value;
        bool         svp_hit;         // tag matching valid entry existed
        bool         confident;       // conf == conf_max at prediction time
    };

    // SVP table, 2^(index_bits) entries, direct mapped
    svp_entry_t *svp;
    unsigned int svp_num_entries;
    unsigned int svp_index_bits;
    unsigned int svp_tag_bits;       // 0 = no tag check
    unsigned int svp_conf_max;

    // VPQ circular buffer, head = oldest (retire side), tail = next free (rename side)
    // Phase bits distinguish full vs empty (same pattern as renamer FL/AL).
    // head == tail && phases match = empty, phases differ = full.
    // Size it large (>= AL size) so it rarely stalls rename. No storage cost.
    vpq_entry_t *vpq;
    unsigned int vpq_size;
    unsigned int vpq_head;
    bool         vpq_head_phase;
    unsigned int vpq_tail;
    bool         vpq_tail_phase;

    // Helpers
    // PC bit layout: [63 ... | tag | index | 0]  (bit 0 discarded, always 0)
    unsigned int get_svp_index(uint64_t pc);   // (pc >> 1) & ((1 << index_bits) - 1)
    uint64_t get_svp_tag(uint64_t pc);         // (pc >> (1 + index_bits)) & ((1 << tag_bits) - 1)
    bool tag_matches(unsigned int svp_index, uint64_t pc); // true if tag_bits==0 or tags equal
    uint64_t count_inflight_instances(unsigned int svp_index); // walk VPQ head to tail counting matches

public:

    // Constructor
    // Maps to: --vp-svp=<vpq_size>,<oracleconf>,<index_bits>,<tag_bits>,<conf_max>
    // oracleconf handled externally in rename.cc, not stored here
    vpu_t(unsigned int vpq_size,
          unsigned int index_bits,
          unsigned int tag_bits,
          unsigned int conf_max);
    ~vpu_t();

    // predict(): called from rename2() per VP-eligible instr
    // Looks up SVP[pc], returns hit/miss.
    // On hit: out_predicted_val = retired_value + instance*stride,
    //         out_confident = (conf == conf_max). Increments instance speculatively.
    // Always allocates a VPQ entry (even on miss, needed for retire training + squash repair).
    // out_vpq_index: store in payload so retire can reference it later.
    // Returns true on SVP hit, false on miss.
    bool predict(uint64_t pc,
                 uint64_t &out_predicted_val,
                 bool &out_confident,
                 unsigned int &out_vpq_index);

    // train(): called from retire.cc per VP-eligible retired instr
    // Uses VPQ entry to train SVP in program order.
    // Tag match: update stride/conf/retired_value, decrement instance.
    // Tag miss: replace entry, init instance by walking VPQ to count in-flight peers.
    // Frees VPQ head after training.
    void train(unsigned int vpq_index, uint64_t committed_val);

    // repair(): called from squash.cc on any squash
    // Walks VPQ backwards from current tail to restored_vpq_tail,
    // decrementing SVP instance counters for each discarded hit entry.
    // For branch misp: restored_vpq_tail = saved at checkpoint time.
    // For squash_complete: restored_vpq_tail = vpq_head (discard everything).
    void repair(unsigned int restored_vpq_tail);

    // vpq_free_entries(): called from rename2() for stall check
    unsigned int vpq_free_entries();

    // get_vpq_tail(): called from rename2() at checkpoint creation
    // Save this alongside branch checkpoint so repair() can restore it on misp.
    unsigned int get_vpq_tail();

    // print_storage(): end of simulation, SVP cost accounting
    // bits/entry = tag + 64(stride) + 64(retired_value) + ceil(log2(vpq_size+1)) + ceil(log2(conf_max+1))
    // VPQ excluded from budget per spec.
    void print_storage(FILE *out);
};

#endif // VPU_H
