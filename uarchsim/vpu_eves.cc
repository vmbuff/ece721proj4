// Project 4 - Competition
#include "vpu_eves.h"
#include <cassert>
#include <cmath>

// Constructor
vpu_eves::vpu_eves(unsigned int vpq_size, unsigned int index_bits, unsigned int tag_bits, unsigned int conf_max,
                   unsigned int denom_intalu, unsigned int denom_fpalu, unsigned int denom_load) {
    // Store configuration parameters and compute derived values
    this->svp_index_bits = index_bits;
    this->svp_tag_bits = tag_bits;
    this->svp_conf_max = conf_max;
    this->svp_num_entries = (1U << index_bits);
    this->vpq_size = vpq_size;

    // Per-instruction-type FPC increment denominators (p = 1/denom)
    p_incr_denom[VPT_INTALU] = denom_intalu;
    p_incr_denom[VPT_FPALU] = denom_fpalu;
    p_incr_denom[VPT_LOAD] = denom_load;

    // SVP table, all entries start invalid
    svp = new svp_entry_t[svp_num_entries];
    for (unsigned int i = 0; i < svp_num_entries; i++) {
        svp[i].tag = 0;
        svp[i].stride = 0;
        svp[i].retired_value = 0;
        svp[i].instance = 0;
        svp[i].conf = 0;
        svp[i].valid = false;
    }

    // VPQ, starts empty (head == tail, same phase)
    vpq = new vpq_entry_t[vpq_size];
    vpq_head = 0;
    vpq_head_phase = false;

    vpq_tail = 0;
    vpq_tail_phase = false;

    // EVES-specific state
    lfsr = 0xACE1;                   // LFSR seed (nonzero so the sequence doesn't stall at 0)
    retire_count = 0;                // Monotonic counter of VP-eligible retired instructions
    last_misp_retire_count = 0;      // retire_count at last confident value mispredict (for cooldown window)
    safestride_total = 0;            // SafeStride: total confident predictions retired
    safestride_miss = 0;             // SafeStride: confident predictions that were wrong
}

// Destructor - frees dynamically allocated SVP table and VPQ buffer
vpu_eves::~vpu_eves() {
   delete[] svp;
   delete[] vpq;
}

// This function extracts the SVP index bits from the PC
unsigned int vpu_eves::get_svp_index(uint64_t pc) {
   return (unsigned int)((pc >> 2) & ((1U << svp_index_bits) - 1));
}

// This function extracts the SVP tag bits from the PC
uint64_t vpu_eves::get_svp_tag(uint64_t pc) {
   uint64_t tag;

   // If there are no tag bits, return 0 (all entries match)
   if (svp_tag_bits == 0) {
      tag = 0;
   // If there are tag bits, extract them from the PC after the index bits
   } else {
      tag = (pc >> (2 + svp_index_bits)) & ((1ULL << svp_tag_bits) - 1);
   }

   return tag;
}

// This function returns true if no tags are used or the stored tag matches the PC tag
bool vpu_eves::tag_matches(unsigned int idx, uint64_t pc) {
   // If there are no tag bits, return true (all entries match)
   if (svp_tag_bits == 0) {
      return true;
   }

   // If the entry is not valid, return false (no match)
   if (!svp[idx].valid) {
      return false;
   }

   // Otherwise, return the result of the tag comparison
   return (svp[idx].tag == get_svp_tag(pc));
}

// Walks VPQ counting in-flight hit entries whose tag still matches
// These are the entries that will decrement instance at retirement
uint64_t vpu_eves::count_inflight_instances(unsigned int svp_index) {
   uint64_t count = 0;                 // Number of in-flight instances found
   unsigned int pos = vpq_head;        // Current VPQ position
   bool phase = vpq_head_phase;        // Current phase bit

   // Walk VPQ from head to tail
   while (!(pos == vpq_tail && phase == vpq_tail_phase)) {
      // Count entry if it maps to this SVP index, was an SVP hit, and tag still matches
      if (vpq[pos].svp_index == svp_index && vpq[pos].svp_hit && tag_matches(svp_index, vpq[pos].pc)) {
         count++;
      }

      pos++;

      // Wrap around and toggle phase if needed
      if (pos == vpq_size) {
         pos = 0;
         phase = !phase;
      }
   }

   return count;
}

// Steps the 16-bit Galois LFSR and returns the new sample used for FPC probability checks
uint16_t vpu_eves::lfsr_step() {
   uint16_t b0 = (lfsr >> 0) & 1;
   uint16_t b2 = (lfsr >> 2) & 1;
   uint16_t b3 = (lfsr >> 3) & 1;
   uint16_t b5 = (lfsr >> 5) & 1;

   uint16_t feedback = b0 ^ b2 ^ b3 ^ b5;
   lfsr = (lfsr >> 1) | (feedback << 15);
   return lfsr;
}

// Returns true if SafeStride kill-switch is currently suppressing stride predictions
bool vpu_eves::safestride_disabled() {
   // Don't engage until enough samples have been collected
   if (safestride_total < SAFESTRIDE_WARMUP) {
      return false;
   }

   // Disable if miss rate exceeds 1/SAFESTRIDE_RATE_DENOM
   return ((uint64_t)safestride_miss * SAFESTRIDE_RATE_DENOM) > (uint64_t)safestride_total;
}


// Returns number of free VPQ entries
unsigned int vpu_eves::vpq_free_entries() {
   // If head and tail point to the same position, phase decides empty vs full
   if (vpq_head == vpq_tail) {
      // Same phase = empty
      if (vpq_head_phase == vpq_tail_phase) {
         return vpq_size;
      // Different phase = full
      } else {
         return 0;
      }
   }
   // Tail ahead of head, occupied span is (tail - head)
   else if (vpq_tail > vpq_head) {
      return vpq_size - (vpq_tail - vpq_head);
   // Tail wrapped behind head, free span is (head - tail)
   } else {
      return vpq_head - vpq_tail;
   }
}

// Getter functions
// Returns current VPQ tail position
unsigned int vpu_eves::get_vpq_tail() {
   return vpq_tail;
}

// Returns current VPQ head position
unsigned int vpu_eves::get_vpq_head() {
   return vpq_head;
}

// Returns current VPQ tail phase
bool vpu_eves::get_vpq_tail_phase() {
   return vpq_tail_phase;
}

// Returns current VPQ head phase
bool vpu_eves::get_vpq_head_phase() {
   return vpq_head_phase;
}

// Looks up SVP by PC - on a hit, computes predicted value and confidence, increments instance
// Always allocates a VPQ entry (even on miss) for retirement training and squash repair
// Suppresses confident predictions during cooldown or SafeStride shutoff
// Returns true on SVP hit, false on miss
bool vpu_eves::predict(uint64_t pc, uint64_t &out_predicted_val, bool &out_confident, unsigned int &out_vpq_index) {
   unsigned int idx = get_svp_index(pc);   // SVP index for this PC
   bool hit;                               // If true, SVP has a usable entry for this PC

   // When tag_bits == 0 the SVP has no tag storage, so every access is a hit
   // Cold accesses read retired_value=stride=0, producing an unconfident zero
   // prediction that retire will score as unconf_corr/incorr (not a miss)
   if (svp_tag_bits == 0) {
      hit = true;
   // Otherwise, a genuine tag miss requires an invalid entry or tag mismatch
   } else {
      hit = svp[idx].valid && tag_matches(idx, pc);
   }

   // Filter block - if SafeStride is active or cooldown is active, still train
   // the predictor but don't inject confident predictions into the pipeline
   bool filter_block = false;
   if (retire_count - last_misp_retire_count < MISP_COOLDOWN) {
      filter_block = true;
   }
   if (safestride_disabled()) {
      filter_block = true;
   }

   // On hit, compute predicted value and confidence
   if (hit) {
      // Increment instance before computing prediction
      // The first in-flight copy should predict retired_value + stride, not retired_value + 0
      svp[idx].instance++;

      out_predicted_val = svp[idx].retired_value + (uint64_t)((int64_t)svp[idx].instance * svp[idx].stride);

      // Confidence requires both saturation AND no active filter block
      bool saturated = (svp[idx].conf >= svp_conf_max);
      out_confident  = saturated && !filter_block;
   }

   // Always allocate a VPQ entry - misses become replacements at retirement,
   // and repair() must walk all entries on squash
   assert(vpq_free_entries() > 0);
   out_vpq_index = vpq_tail;

   vpq[vpq_tail].pc = pc;
   vpq[vpq_tail].svp_index = idx;
   vpq[vpq_tail].svp_hit = hit;

   // On hit, store the predicted value and confidence for retire to score
   if (hit) {
      vpq[vpq_tail].predicted_value = out_predicted_val;
      vpq[vpq_tail].confident = out_confident;
   // On miss, store safe defaults - retire treats this as an SVP miss
   } else {
      vpq[vpq_tail].predicted_value = 0;
      vpq[vpq_tail].confident = false;
   }

   // Advance tail, wrapping and toggling phase if needed
   vpq_tail++;
   if (vpq_tail == vpq_size) {
      vpq_tail = 0;
      vpq_tail_phase = !vpq_tail_phase;
   }

   return hit;
}


// Trains SVP in program order using committed value from PRF
// Tag match: updates stride, conf (via FPC), retired_value, decrements instance (if svp_hit)
// Tag miss: replaces SVP entry and initializes instance by counting in-flight peers in VPQ
// Also updates EVES state: retire_count, SafeStride counters, and cooldown timestamp
// Frees VPQ head entry after training
void vpu_eves::train(unsigned int vpq_index, uint64_t committed_val, uint8_t inst_type) {
   assert(vpq_index == vpq_head);                        // Must always train the head entry (in-order retirement)

   unsigned int idx = vpq[vpq_index].svp_index;          // SVP index cached at predict time
   uint64_t pc = vpq[vpq_index].pc;                      // Original PC for tag comparison
   uint64_t predicted_val = vpq[vpq_index].predicted_value;
   bool was_confident_pred = vpq[vpq_index].confident;

   retire_count++;

   // Halve the SafeStride counters every RESET_PERIOD so a bad phase doesn't
   // permanently disable the stride predictor
   if ((retire_count % SAFESTRIDE_RESET_PERIOD) == 0) {
      safestride_total = safestride_total >> 1;
      safestride_miss  = safestride_miss >> 1;
   }

   // Track SafeStride rate and cooldown trigger on confident predictions
   if (was_confident_pred) {
      if (safestride_total < UINT32_MAX) {
         safestride_total++;
      }

      // Confident mispredict - bump miss counter and reset cooldown window
      if (committed_val != predicted_val) {
         if (safestride_miss < UINT32_MAX) {
            safestride_miss++;
         }
         last_misp_retire_count = retire_count;
      }
   }

   // Tag match - update stride and confidence per spec
   if (svp[idx].valid && tag_matches(idx, pc)) {
      int64_t new_stride = (int64_t)(committed_val - svp[idx].retired_value);

      // Stride confirmed, probabilistically increment confidence via FPC
      if (new_stride == svp[idx].stride) {
         uint8_t t = (inst_type < VPT_COUNT) ? inst_type : VPT_INTALU;   // Fallback to INTALU on unknown type
         uint16_t sample = lfsr_step();

         // Only bump conf with probability 1/denom for this instruction type
         if ((sample % p_incr_denom[t]) == 0) {
            if (svp[idx].conf < svp_conf_max) {
               svp[idx].conf++;
            }
         }
      // Stride changed, adopt new stride and reset confidence
      } else {
         svp[idx].stride = new_stride;
         svp[idx].conf = 0;
      }

      svp[idx].retired_value = committed_val;

      // Only decrement instance if this entry was an SVP hit at predict time
      // If it was a miss, no instance++ happened so no decrement is needed
      if (vpq[vpq_index].svp_hit) {
         assert(svp[idx].instance > 0);
         svp[idx].instance--;
      }
   // Tag miss or invalid, replace entry per spec
   } else {
      svp[idx].tag = get_svp_tag(pc);
      svp[idx].retired_value = committed_val;
      svp[idx].stride = (int64_t)committed_val;             // retired_value = stride = value per spec
      svp[idx].conf = 0;
      svp[idx].valid = true;

      // Temporarily advance head past the entry being freed so
      // count_inflight_instances excludes it from the count
      unsigned int save_head = vpq_head;                    // Save current head for restore
      bool save_phase = vpq_head_phase;                     // Save current phase for restore

      vpq_head++;

      // Wrap around and toggle phase if needed
      if (vpq_head == vpq_size) {
         vpq_head = 0;
         vpq_head_phase = !vpq_head_phase;
      }

      svp[idx].instance = count_inflight_instances(idx);
      vpq_head = save_head;
      vpq_head_phase = save_phase;
   }

   // Free VPQ head by advancing past the trained entry
   vpq_head++;

   // Wrap around and toggle phase if needed
   if (vpq_head == vpq_size) {
      vpq_head = 0;
      vpq_head_phase = !vpq_head_phase;
   }
}


// Walks VPQ tail back to the restored checkpoint, decrementing SVP instance counters along the way
// Both position and phase must match, the VPQ can wrap a full vpq_size back to the same position
// with the phase flipped
void vpu_eves::repair(unsigned int restored_vpq_tail, bool restored_vpq_tail_phase) {
   // Walk backwards until the restored position and phase are reached
   while (vpq_tail != restored_vpq_tail || vpq_tail_phase != restored_vpq_tail_phase) {
      // Step backward, wrapping and toggling phase at the buffer boundary
      if (vpq_tail == 0) {
         vpq_tail = vpq_size - 1;
         vpq_tail_phase = !vpq_tail_phase;
      } else {
         vpq_tail--;
      }

      // Decrement instance only if tag still matches, otherwise the SVP entry
      // was replaced after this instruction's predict and we'd corrupt the new entry
      if (vpq[vpq_tail].svp_hit && tag_matches(vpq[vpq_tail].svp_index, vpq[vpq_tail].pc)) {
         assert(svp[vpq[vpq_tail].svp_index].instance > 0);
         svp[vpq[vpq_tail].svp_index].instance--;
      }
   }
}

// Computes and prints SVP + EVES storage cost accounting to the stats log
// VPQ is excluded from the storage budget per spec
void vpu_eves::print_storage(FILE *out) {
   unsigned int instance_bits;
   if (vpq_size > 0) {
      instance_bits = (unsigned int)ceil(log2((double)vpq_size));
   } else {
      instance_bits = 1;
   }

   unsigned int conf_bits = (unsigned int)ceil(log2((double)(svp_conf_max + 1)));
   unsigned int bits_per_entry = svp_tag_bits + conf_bits + 64 + 64 + instance_bits;
   unsigned int svp_total_bits = svp_num_entries * bits_per_entry;

   // EVES adds LFSR state plus two SafeStride counters on top of the SVP fields
   // retire_count is not counted (same convention as the VPQ pointers)
   unsigned int eves_overhead_bits = 16 + 32 + 32;
   unsigned int total_bits = svp_total_bits + eves_overhead_bits;
   double total_bytes = total_bits / 8.0;
   double total_kb = total_bytes / 1024.0;

   fprintf(out, "   EVES predictor storage (SVP core + EVES overhead):\n");
   fprintf(out, "   FPC denominators: intalu=%u, fpalu=%u, load=%u\n",
           p_incr_denom[VPT_INTALU], p_incr_denom[VPT_FPALU], p_incr_denom[VPT_LOAD]);
   fprintf(out, "   One SVP entry:\n");
   fprintf(out, "      tag           : %3u bits  // num_tag_bits\n", svp_tag_bits);
   fprintf(out, "      conf          : %3u bits  // formula: (uint64_t)ceil(log2((double)(confmax+1)))\n", conf_bits);
   fprintf(out, "      retired_value :  64 bits  // RISCV64 integer size.\n");
   fprintf(out, "      stride        :  64 bits  // RISCV64 integer size. Competition opportunity: truncate stride to far fewer bits based on stride distribution of stride-predictable instructions.\n");
   fprintf(out, "      instance ctr  : %3u bits  // formula: (uint64_t)ceil(log2((double)VPQsize))\n", instance_bits);
   fprintf(out, "      -------------------------\n");
   fprintf(out, "      bits/SVP entry: %u bits\n", bits_per_entry);
   fprintf(out, "   SVP core storage = (%u SVP entries x %u bits/SVP entry) = %u bits\n", svp_num_entries, bits_per_entry, svp_total_bits);
   fprintf(out, "   EVES overhead    = LFSR(16) + SafeStride_total(32) + SafeStride_miss(32) = %u bits\n", eves_overhead_bits);
   fprintf(out, "   Total storage cost (bits)  = %u bits\n", total_bits);
   fprintf(out, "   Total storage cost (bytes) = %.2f B (%.2f KB)\n", total_bytes, total_kb);
}
