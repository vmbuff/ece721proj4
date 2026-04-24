#include "vpu_eves.h"
#include <cassert>
#include <cmath>

// Per-instruction-type FPC increment denominators. Index == vp_inst_type enum,
// which mirrors the three eligibility booleans in parameters.h
// (predINTALU / predFPALU / predLOAD). Loads share one bucket -- no
// LLC-miss vs L1-hit distinction (cache-hint plumbing is out of scope).
//
// p = 1/DENOM. All powers of 2 so the probabilistic check reduces to an
// AND-mask on the 16-bit LFSR, matching the cheap-hardware assumption in
// Perais & Seznec HPCA14.
static const uint16_t P_INCR_DENOM[VPT_COUNT] = {
    128,  // VPT_INTALU  (single-cycle / LONGLAT ints -- largest demotion, biggest lever)
     32,  // VPT_FPALU   (slower ALU / FP -- moderate demotion)
      8,  // VPT_LOAD    (highest benefit -- least demotion)
};

vpu_eves::vpu_eves(unsigned int vpq_size, unsigned int index_bits, unsigned int tag_bits, unsigned int conf_max) {
    svp_index_bits  = index_bits;
    svp_tag_bits    = tag_bits;
    svp_conf_max    = conf_max;
    svp_num_entries = (1U << index_bits);
    this->vpq_size  = vpq_size;

    svp = new svp_entry_t[svp_num_entries];
    for (unsigned int i = 0; i < svp_num_entries; i++) {
        svp[i].tag           = 0;
        svp[i].stride        = 0;
        svp[i].retired_value = 0;
        svp[i].instance      = 0;
        svp[i].conf          = 0;
        svp[i].valid         = false;
    }

    vpq = new vpq_entry_t[vpq_size];
    vpq_head       = 0;
    vpq_head_phase = false;
    vpq_tail       = 0;
    vpq_tail_phase = false;

    lfsr                    = 0xACE1u;  // non-zero seed
    retire_count            = 0;
    last_misp_retire_count  = 0;

    safestride_total = 0;
    safestride_miss  = 0;
}

vpu_eves::~vpu_eves() {
    delete[] svp;
    delete[] vpq;
}

unsigned int vpu_eves::get_svp_index(uint64_t pc) {
    return (unsigned int)((pc >> 2) & ((1U << svp_index_bits) - 1));
}

uint64_t vpu_eves::get_svp_tag(uint64_t pc) {
    if (svp_tag_bits == 0) return 0;
    return (pc >> (2 + svp_index_bits)) & ((1ULL << svp_tag_bits) - 1);
}

bool vpu_eves::tag_matches(unsigned int idx, uint64_t pc) {
    if (svp_tag_bits == 0) return true;
    if (!svp[idx].valid)   return false;
    return (svp[idx].tag == get_svp_tag(pc));
}

uint64_t vpu_eves::count_inflight_instances(unsigned int svp_index) {
    uint64_t count = 0;
    unsigned int pos = vpq_head;
    bool phase = vpq_head_phase;

    while (!(pos == vpq_tail && phase == vpq_tail_phase)) {
        if (vpq[pos].svp_index == svp_index && vpq[pos].svp_hit && tag_matches(svp_index, vpq[pos].pc)) {
            count++;
        }
        pos++;
        if (pos == vpq_size) { pos = 0; phase = !phase; }
    }
    return count;
}

// Galois LFSR with taps at bits 16, 14, 13, 11 (period 2^16 - 1).
uint16_t vpu_eves::lfsr_step() {
    uint16_t bit = ((lfsr >> 0) ^ (lfsr >> 2) ^ (lfsr >> 3) ^ (lfsr >> 5)) & 1u;
    lfsr = (lfsr >> 1) | (uint16_t)(bit << 15);
    return lfsr;
}

bool vpu_eves::safestride_disabled() const {
    if (safestride_total < SAFESTRIDE_WARMUP) return false;
    // Suppress when miss * DENOM > total  (i.e., rate > 1/DENOM).
    // Using 64-bit arithmetic to avoid overflow at 32 bits worst case.
    return ((uint64_t)safestride_miss * SAFESTRIDE_RATE_DENOM) > (uint64_t)safestride_total;
}

unsigned int vpu_eves::vpq_free_entries() {
    if (vpq_head == vpq_tail) {
        return (vpq_head_phase == vpq_tail_phase) ? vpq_size : 0;
    } else if (vpq_tail > vpq_head) {
        return vpq_size - (vpq_tail - vpq_head);
    } else {
        return vpq_head - vpq_tail;
    }
}

unsigned int vpu_eves::get_vpq_tail()       { return vpq_tail; }
unsigned int vpu_eves::get_vpq_head()       { return vpq_head; }
bool         vpu_eves::get_vpq_tail_phase() { return vpq_tail_phase; }
bool         vpu_eves::get_vpq_head_phase() { return vpq_head_phase; }

bool vpu_eves::predict(uint64_t pc, uint64_t &out_predicted_val, bool &out_confident, unsigned int &out_vpq_index) {
    unsigned int idx = get_svp_index(pc);

    // Hit semantics are identical to the baseline SVP (see vpu.cc comment).
    bool hit;
    if (svp_tag_bits == 0) {
        hit = true;
    } else {
        hit = svp[idx].valid && tag_matches(idx, pc);
    }

    // EVES filters operate on top of the hit computation. If either the
    // cooldown or the SafeStride kill-switch is active, the VPQ entry is
    // still allocated and the instance counter is still incremented (so
    // repair/train continue to balance), but the emitted confidence is
    // forced to false so the pipeline does not inject the prediction.
    bool filter_block = false;
    if (retire_count - last_misp_retire_count < MISP_COOLDOWN) filter_block = true;
    if (safestride_disabled())                                 filter_block = true;

    if (hit) {
        svp[idx].instance++;
        out_predicted_val = svp[idx].retired_value + (uint64_t)((int64_t)svp[idx].instance * svp[idx].stride);
        bool saturated    = (svp[idx].conf >= svp_conf_max);
        out_confident     = saturated && !filter_block;
    }

    assert(vpq_free_entries() > 0);
    out_vpq_index = vpq_tail;

    vpq[vpq_tail].pc        = pc;
    vpq[vpq_tail].svp_index = idx;
    vpq[vpq_tail].svp_hit   = hit;
    if (hit) {
        vpq[vpq_tail].predicted_value = out_predicted_val;
        vpq[vpq_tail].confident       = out_confident;
    } else {
        vpq[vpq_tail].predicted_value = 0;
        vpq[vpq_tail].confident       = false;
    }

    vpq_tail++;
    if (vpq_tail == vpq_size) { vpq_tail = 0; vpq_tail_phase = !vpq_tail_phase; }

    return hit;
}

void vpu_eves::train(unsigned int vpq_index, uint64_t committed_val, uint8_t inst_type) {
    assert(vpq_index == vpq_head);

    unsigned int idx = vpq[vpq_index].svp_index;
    uint64_t pc      = vpq[vpq_index].pc;
    bool     was_confident_pred = vpq[vpq_index].confident;
    uint64_t predicted_val      = vpq[vpq_index].predicted_value;

    retire_count++;

    // Periodic SafeStride decay - divide counters by 2 every RESET_PERIOD
    // so a transient bad region doesn't kill the stride predictor forever.
    if ((retire_count % SAFESTRIDE_RESET_PERIOD) == 0) {
        safestride_total >>= 1;
        safestride_miss  >>= 1;
    }

    // SafeStride: count every emitted prediction into total regardless of
    // correctness. This measures rate across ALL PCs jointly - EVES's
    // per-trace adaptive signal, not per-entry.
    if (was_confident_pred) {
        if (safestride_total < UINT32_MAX) safestride_total++;
        if (committed_val != predicted_val) {
            if (safestride_miss < UINT32_MAX) safestride_miss++;
            last_misp_retire_count = retire_count;        // arm 128-insn cooldown
        }
    }

    if (svp[idx].valid && tag_matches(idx, pc)) {
        int64_t new_stride = (int64_t)(committed_val - svp[idx].retired_value);

        if (new_stride == svp[idx].stride) {
            // Forward Probabilistic Counter: increment only when LFSR draws
            // a sample in the 0 bucket of a DENOM-wide uniform partition.
            // Because DENOM is a power of 2, (sample & (DENOM-1)) == 0 is
            // a uniform 1/DENOM Bernoulli trial.
            uint8_t t = (inst_type < VPT_COUNT) ? inst_type : VPT_INTALU;
            uint16_t sample = lfsr_step();
            if ((sample & (P_INCR_DENOM[t] - 1)) == 0) {
                if (svp[idx].conf < svp_conf_max) svp[idx].conf++;
            }
        } else {
            svp[idx].stride = new_stride;
            svp[idx].conf   = 0;
        }

        svp[idx].retired_value = committed_val;

        if (vpq[vpq_index].svp_hit) {
            assert(svp[idx].instance > 0);
            svp[idx].instance--;
        }
    } else {
        svp[idx].tag           = get_svp_tag(pc);
        svp[idx].retired_value = committed_val;
        svp[idx].stride        = (int64_t)committed_val;
        svp[idx].conf          = 0;
        svp[idx].valid         = true;

        unsigned int save_head  = vpq_head;
        bool         save_phase = vpq_head_phase;

        vpq_head++;
        if (vpq_head == vpq_size) { vpq_head = 0; vpq_head_phase = !vpq_head_phase; }

        svp[idx].instance = count_inflight_instances(idx);
        vpq_head       = save_head;
        vpq_head_phase = save_phase;
    }

    vpq_head++;
    if (vpq_head == vpq_size) { vpq_head = 0; vpq_head_phase = !vpq_head_phase; }
}

void vpu_eves::repair(unsigned int restored_vpq_tail, bool restored_vpq_tail_phase) {
    while (vpq_tail != restored_vpq_tail || vpq_tail_phase != restored_vpq_tail_phase) {
        if (vpq_tail == 0) {
            vpq_tail = vpq_size - 1;
            vpq_tail_phase = !vpq_tail_phase;
        } else {
            vpq_tail--;
        }

        if (vpq[vpq_tail].svp_hit && tag_matches(vpq[vpq_tail].svp_index, vpq[vpq_tail].pc)) {
            assert(svp[vpq[vpq_tail].svp_index].instance > 0);
            svp[vpq[vpq_tail].svp_index].instance--;
        }
    }
}

void vpu_eves::print_storage(FILE *out) {
    unsigned int instance_bits;
    if (vpq_size > 0) {
        instance_bits = (unsigned int)ceil(log2((double)vpq_size));
    } else {
        instance_bits = 1;
    }

    unsigned int conf_bits      = (unsigned int)ceil(log2((double)(svp_conf_max + 1)));
    unsigned int bits_per_entry = svp_tag_bits + conf_bits + 64 + 64 + instance_bits;
    unsigned int svp_total_bits = svp_num_entries * bits_per_entry;

    // EVES overhead: 16-bit LFSR + two 32-bit SafeStride counters +
    // misprediction-date counter (reuse of existing 64-bit retire_count is
    // an implementation-only counter and excluded, same rule as VPQ pointers).
    unsigned int eves_overhead_bits = 16 + 32 + 32;

    unsigned int total_bits  = svp_total_bits + eves_overhead_bits;
    double       total_bytes = total_bits / 8.0;
    double       total_kb    = total_bytes / 1024.0;

    fprintf(out, "   EVES predictor storage accounting (SVP core + EVES overhead):\n");
    fprintf(out, "   One SVP entry:\n");
    fprintf(out, "      tag           : %3u bits\n", svp_tag_bits);
    fprintf(out, "      conf          : %3u bits\n", conf_bits);
    fprintf(out, "      retired_value :  64 bits\n");
    fprintf(out, "      stride        :  64 bits\n");
    fprintf(out, "      instance ctr  : %3u bits\n", instance_bits);
    fprintf(out, "      -------------------------\n");
    fprintf(out, "      bits/SVP entry: %u bits\n", bits_per_entry);
    fprintf(out, "   SVP core: %u entries x %u bits = %u bits\n", svp_num_entries, bits_per_entry, svp_total_bits);
    fprintf(out, "   EVES overhead:\n");
    fprintf(out, "      LFSR state         : 16 bits\n");
    fprintf(out, "      SafeStride total   : 32 bits\n");
    fprintf(out, "      SafeStride miss    : 32 bits\n");
    fprintf(out, "      EVES overhead total: %u bits\n", eves_overhead_bits);
    fprintf(out, "   Total storage cost (bits)  = %u bits\n", total_bits);
    fprintf(out, "   Total storage cost (bytes) = %.2f B (%.2f KB)\n", total_bytes, total_kb);
}
