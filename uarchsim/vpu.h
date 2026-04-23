// Project 4 - Value Prediction
#ifndef VPU_H
#define VPU_H

// Include statements
#include <cstdint>
#include <cstdio>

// VPU Class
// Creates the Strided Value Predictor (SVP) and Value Prediction Queue (VPQ) structures
class vpu {
private:
    // SVP entry
    // Prediction formula: retired_value + instance * stride
    // Instance is speculatively incremented at Rename and decremented at retirement
    // conf is a saturating counter — prediction is only injected when conf == conf_max
    struct svp_entry_t {
        uint64_t     tag;
        int64_t      stride;            // Signed per spec
        uint64_t     retired_value;     // Last committed destination value for this PC
        uint64_t     instance;          // Number of in-flight copies of this PC (speculative)
        unsigned int conf;              // Confidence counter
        bool         valid;             // If true, this SVP entry contains valid data
    };

    // VPQ entry
    // Allocated at Rename (tail) and freed at retirement (head)
    // Stores enough information to train the SVP at retirement and compute vpmeas stats
    struct vpq_entry_t {
        uint64_t     pc;
        unsigned int svp_index;         // SVP index for this PC, cached to avoid recomputation at retirement
        uint64_t     predicted_value;
        bool         svp_hit;           // If true, SVP had a valid tag-matching entry at prediction time
        bool         confident;         // If true, conf == conf_max at prediction time
    };

    // SVP table
    // Is direct-mapped
    svp_entry_t *svp;
    unsigned int svp_num_entries;
    unsigned int svp_index_bits;
    unsigned int svp_tag_bits;          // 0 = no tag check
    unsigned int svp_conf_max;

    // VPQ circular buffer
    // head = oldest entry (retirement side), tail = next free slot (Rename side)
    // Phase bits distinguish full from empty when head == tail (same pattern as renamer FL/AL)
    vpq_entry_t *vpq;
    unsigned int vpq_size;
    unsigned int vpq_head;
    bool         vpq_head_phase;
    unsigned int vpq_tail;
    bool         vpq_tail_phase;

    // Extracts SVP index bits from PC to determine which SVP entry this instruction maps to
    unsigned int get_svp_index(uint64_t pc);

    // Extracts SVP tag bits from PC to identify the instruction within its SVP entry
    uint64_t get_svp_tag(uint64_t pc);

    // Returns true if no tags are used (svp_tag_bits == 0) or the stored tag matches the PC tag
    bool tag_matches(unsigned int idx, uint64_t pc);

    // Walks VPQ from head to tail counting in-flight entries that will decrement
    // instance at retirement — only counts hit entries whose tag still matches
    uint64_t count_inflight_instances(unsigned int svp_index);

public:
    // Constructor — parameters map to --vp-svp=<vpq_size>,<oracleconf>,<index_bits>,<tag_bits>,<conf_max>
    // oracleconf is handled externally in rename.cc and not stored here
    vpu(unsigned int vpq_size,
        unsigned int index_bits,
        unsigned int tag_bits,
        unsigned int conf_max);
    ~vpu();

    // Called from rename2() per VP-eligible instruction
    // Looks up SVP by PC — on hit, computes predicted value and confidence, increments instance
    // Always allocates a VPQ entry (even on miss) for retirement training and squash repair
    // Returns true on SVP hit, false on miss
    bool predict(uint64_t pc,
                    uint64_t &out_predicted_val,
                    bool &out_confident,
                    unsigned int &out_vpq_index);

    // Called from retire.cc per VP-eligible retired instruction
    // Trains SVP in program order using committed value from PRF
    // Tag match: updates stride, conf, retired_value, decrements instance
    // Tag miss: replaces entry, initializes instance by counting in-flight peers in VPQ
    // Frees VPQ head entry after training
    void train(unsigned int vpq_index, uint64_t committed_val);

    // Called from squash.cc on any pipeline squash
    // Walks VPQ backwards from current tail to (restored_tail, restored_tail_phase),
    // decrementing SVP instance counters for each discarded hit entry
    // Both position AND phase are required — VPQ can wrap a full vpq_size back to the
    // same position with the phase flipped, making a position-only check incorrect
    void repair(unsigned int restored_vpq_tail, bool restored_vpq_tail_phase);

    // Called from rename2() to check if VPQ has enough free entries for the rename bundle
    unsigned int vpq_free_entries();

    // Called from rename2() at branch checkpoint creation — save alongside branch_ID
    unsigned int get_vpq_tail();
    bool         get_vpq_tail_phase();

    // Called from squash.cc for squash_complete — repair target to discard all in-flight entries
    unsigned int get_vpq_head();
    bool         get_vpq_head_phase();

    // Called at end of simulation — prints SVP storage cost accounting to stats log
    // VPQ is excluded from the storage budget per spec
    void print_storage(FILE *out);
};

#endif // VPU_H
