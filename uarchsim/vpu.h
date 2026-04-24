// Project 4 - Value Prediction
#ifndef VPU_H
#define VPU_H

// Include statements
#include <cstdint>
#include <cstdio>

#include "vpu_iface.h"

// Creates the Strided Value Predictor (SVP) and Value Prediction Queue (VPQ) structures
class vpu : public vpu_iface {
private:
    // SVP entry - prediction formula: retired_value + instance * stride
    // Instance is speculatively incremented at Rename and decremented at retirement
    struct svp_entry_t {
        uint64_t     tag;
        int64_t      stride;            // Signed per spec
        uint64_t     retired_value;     // Last committed destination value for this PC
        uint64_t     instance;          // Number of in-flight copies of this PC (speculative)
        unsigned int conf;              // Confidence counter (prediction injected when conf == conf_max)
        bool         valid;             // If true, this SVP entry contains valid data
    };

    // VPQ entry - allocated at Rename (tail), freed at retirement (head)
    // Stores enough info to train the SVP at retirement and compute vpmeas stats
    struct vpq_entry_t {
        uint64_t     pc;
        unsigned int svp_index;         // Cached SVP index to avoid recomputation at retirement
        uint64_t     predicted_value;
        bool         svp_hit;           // If true, SVP had a valid tag-matching entry at prediction time
        bool         confident;         // If true, conf = conf_max at prediction time
    };

    // SVP table (direct-mapped)
    svp_entry_t *svp;
    unsigned int svp_num_entries;
    unsigned int svp_index_bits;
    unsigned int svp_tag_bits;          // 0 = no tag check
    unsigned int svp_conf_max;

    // VPQ circular buffer - head = oldest entry, tail = next free slot
    // Phase bits distinguish full from empty when head == tail
    vpq_entry_t *vpq;
    unsigned int vpq_size;
    unsigned int vpq_head;
    bool         vpq_head_phase;
    unsigned int vpq_tail;
    bool         vpq_tail_phase;

    // Extracts SVP index bits from PC
    unsigned int get_svp_index(uint64_t pc);

    // Extracts SVP tag bits from PC
    uint64_t get_svp_tag(uint64_t pc);

    // Returns true if no tags are used or the stored tag matches the PC tag
    bool tag_matches(unsigned int idx, uint64_t pc);

    // Walks VPQ counting in-flight hit entries that will decrement instance at retirement
    uint64_t count_inflight_instances(unsigned int svp_index);

public:
    // Constructor
    vpu(unsigned int vpq_size, unsigned int index_bits, unsigned int tag_bits, unsigned int conf_max);
    ~vpu();
    vpu(const vpu&) = delete;
    vpu& operator=(const vpu&) = delete;

    // Called from rename2() per VP-eligible instruction
    // On SVP hit: computes predicted value, increments instance, always allocates a VPQ entry
    bool predict(uint64_t pc, uint64_t &out_predicted_val, bool &out_confident, unsigned int &out_vpq_index) override;

    // Called from retire.cc per VP-eligible retired instruction
    // Trains SVP in program order and frees the VPQ head entry, inst_type is unused here (EVES uses it)
    void train(unsigned int vpq_index, uint64_t committed_val, uint8_t inst_type) override;

    // Called from squash.cc on any pipeline squash
    // Walks VPQ tail back to the restored checkpoint, decrementing instance counters along the way
    void repair(unsigned int restored_vpq_tail, bool restored_vpq_tail_phase) override;

    // Called from rename2() to check if VPQ has enough free entries for the rename bundle
    unsigned int vpq_free_entries() override;

    // Called from rename2() at branch checkpoint creation, saved with branch_ID
    unsigned int get_vpq_tail() override;
    bool get_vpq_tail_phase() override;

    // Called from squash.cc for squash_complete, repair target to discard all in-flight entries
    unsigned int get_vpq_head() override;
    bool get_vpq_head_phase() override;

    // Called at end of simulation, prints SVP storage cost accounting 
    void print_storage(FILE *out) override;
};

#endif // VPU_H
