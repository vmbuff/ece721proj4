// Project 4 - Value Prediction
#include "vpu.h"
#include <cassert>
#include <cmath>

// Constructor
vpu::vpu(unsigned int vpq_size, unsigned int index_bits, unsigned int tag_bits, unsigned int conf_max) {
   // Store configuration parameters and compute derived values
   this->svp_index_bits  = index_bits;
   this->svp_tag_bits    = tag_bits;
   this->svp_conf_max    = conf_max;
   this->svp_num_entries = (1U << index_bits);
   this->vpq_size        = vpq_size;

   // SVP table, all entries start invalid
   svp = new svp_entry_t[svp_num_entries];
   for (unsigned int i = 0; i < svp_num_entries; i++) {
      svp[i].tag           = 0;
      svp[i].stride        = 0;
      svp[i].retired_value = 0;
      svp[i].instance      = 0;
      svp[i].conf          = 0;
      svp[i].valid         = false;
   }

   // VPQ, starts empty (head == tail, same phase)
   vpq = new vpq_entry_t[vpq_size];
   vpq_head       = 0;
   vpq_head_phase = false;
   vpq_tail       = 0;
   vpq_tail_phase = false;
}

// Destructor - frees dynamically allocated SVP table and VPQ buffer
vpu::~vpu() {
   delete[] svp;
   delete[] vpq;
}

// This function extracts the SVP index bits from the PC
unsigned int vpu::get_svp_index(uint64_t pc) {
   return (unsigned int)((pc >> 2) & ((1U << svp_index_bits) - 1));
}

// This function extracts the SVP tag bits from the PC
uint64_t vpu::get_svp_tag(uint64_t pc) {
   // If there are no tag bits, return 0 (all entries match)
   if (svp_tag_bits == 0) {
      return 0;
   }

   // If there are tag bits, extract them from the PC after the index bits
   return (pc >> (2 + svp_index_bits)) & ((1ULL << svp_tag_bits) - 1);
}

// This function returns true if no tags are used or the stored tag matches the PC tag
bool vpu::tag_matches(unsigned int idx, uint64_t pc) {
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

// Walks VPQ from head to tail counting in-flight entries that will decrement
// instance at retirement. Only counts entries where svp_hit is true AND
// the tag still matches - entries that fail either condition will not
// decrement instance at train time and must not be counted here
uint64_t vpu::count_inflight_instances(unsigned int svp_index) {
   uint64_t count = 0;
   unsigned int pos = vpq_head;
   bool phase = vpq_head_phase;

   while (!(pos == vpq_tail && phase == vpq_tail_phase)) {
      if (vpq[pos].svp_index == svp_index && vpq[pos].svp_hit && tag_matches(svp_index, vpq[pos].pc)) {
         count++;
      }
      pos++;
      if (pos == vpq_size) { 
         pos = 0; 
         phase = !phase; 
      }
   }
   return count;
}


// Returns number of free VPQ entries using phase-bit arithmetic (same pattern as renamer FL/AL)
unsigned int vpu::vpq_free_entries() {
   if (vpq_head == vpq_tail) {
      // Same position - same phase = empty, different phase = full
      return (vpq_head_phase == vpq_tail_phase) ? vpq_size : 0;
   }
   else if (vpq_tail > vpq_head) {
      return vpq_size - (vpq_tail - vpq_head);
   }
   else {
      return vpq_head - vpq_tail;
   }
}

// Getter functions
// Returns current VPQ tail position
unsigned int vpu::get_vpq_tail() { 
   return vpq_tail; 
}

// Returns current VPQ head position
unsigned int vpu::get_vpq_head() { 
   return vpq_head; 
}

// Returns current VPQ tail phase
bool vpu::get_vpq_tail_phase() { 
   return vpq_tail_phase; 
}

// Returns current VPQ head phase
bool vpu::get_vpq_head_phase() { 
   return vpq_head_phase; 
}

// Looks up SVP by PC - on hit, computes predicted value and confidence, increments instance
// Always allocates a VPQ entry (even on miss) for retirement training and squash repair
// Returns true on SVP hit, false on miss
bool vpu::predict(uint64_t pc, uint64_t &out_predicted_val, bool &out_confident, unsigned int &out_vpq_index) {
   unsigned int idx = get_svp_index(pc);
   bool hit = (svp[idx].valid && tag_matches(idx, pc));

   if (hit) {
      // Increment instance before computing prediction - the first in-flight
      // copy should predict retired_value + stride, not retired_value + 0
      svp[idx].instance++;

      out_predicted_val = svp[idx].retired_value + (uint64_t)((int64_t)svp[idx].instance * svp[idx].stride);
      out_confident = (svp[idx].conf >= svp_conf_max);
   }

   // Always allocate a VPQ entry - misses become replacements at retirement,
   // and repair() must walk all entries on squash
   assert(vpq_free_entries() > 0);
   out_vpq_index = vpq_tail;

   vpq[vpq_tail].pc              = pc;
   vpq[vpq_tail].svp_index       = idx;
   vpq[vpq_tail].predicted_value = hit ? out_predicted_val : 0;
   vpq[vpq_tail].svp_hit         = hit;
   vpq[vpq_tail].confident       = hit ? out_confident : false;

   // Advance tail
   vpq_tail++;
   if (vpq_tail == vpq_size) { 
      vpq_tail = 0; 
      vpq_tail_phase = !vpq_tail_phase; 
   }

   return hit;
}


// Trains SVP in program order using committed value from PRF
// Tag match: updates stride, conf, retired_value, decrements instance (if svp_hit)
// Tag miss: replaces SVP entry and initializes instance by counting in-flight peers in VPQ
// Frees VPQ head entry after training
void vpu::train(unsigned int vpq_index, uint64_t committed_val) {
   assert(vpq_index == vpq_head); // Must always train the head entry (in-order retirement)

   unsigned int idx = vpq[vpq_index].svp_index;
   uint64_t pc = vpq[vpq_index].pc;

   if (svp[idx].valid && tag_matches(idx, pc)) {
      // Tag match - update stride and confidence per spec:
      // new_stride = value - retired_value
      // if new_stride == stride: conf++ (saturate at conf_max)
      // else: stride = new_stride, conf = 0
      int64_t new_stride = (int64_t)(committed_val - svp[idx].retired_value);

      if (new_stride == svp[idx].stride) {
         if (svp[idx].conf < svp_conf_max) svp[idx].conf++;
      }
      else {
         svp[idx].stride = new_stride;
         svp[idx].conf   = 0;
      }

      svp[idx].retired_value = committed_val;

      // Only decrement instance if this entry was an SVP hit at predict time -
      // if it was a miss, no instance++ happened so no decrement is needed
      if (vpq[vpq_index].svp_hit) {
         assert(svp[idx].instance > 0);
         svp[idx].instance--;
      }
   }
   else {
      // Tag miss or invalid - replace entry per spec:
      // retired_value = stride = value, conf = 0, tag = PCtag
      svp[idx].tag           = get_svp_tag(pc);
      svp[idx].retired_value = committed_val;
      svp[idx].stride        = (int64_t)committed_val;
      svp[idx].conf          = 0;
      svp[idx].valid         = true;

      // Temporarily advance head past the entry being freed so
      // count_inflight_instances excludes it from the count
      unsigned int save_head  = vpq_head;
      bool         save_phase = vpq_head_phase;

      vpq_head++;

      if (vpq_head == vpq_size) { 
         vpq_head = 0; 
         vpq_head_phase = !vpq_head_phase; 
      }

      svp[idx].instance = count_inflight_instances(idx);
      vpq_head       = save_head;
      vpq_head_phase = save_phase;
   }

   // Free VPQ head
   vpq_head++;

   if (vpq_head == vpq_size) { 
      vpq_head = 0; 
      vpq_head_phase = !vpq_head_phase; 
   }
}


// Walks VPQ backwards from current tail to (restored_tail, restored_tail_phase),
// decrementing SVP instance counters for each discarded hit entry
// Both position AND phase are compared - VPQ can wrap a full vpq_size back to
// the same position with the phase flipped, making a position-only check incorrect
void vpu::repair(unsigned int restored_vpq_tail, bool restored_vpq_tail_phase) {
   while (vpq_tail != restored_vpq_tail || vpq_tail_phase != restored_vpq_tail_phase) {
      
      // Step backward
      if (vpq_tail == 0) { 
         vpq_tail = vpq_size - 1; 
         vpq_tail_phase = !vpq_tail_phase; 
      }

      else vpq_tail--;

      // Only decrement instance if the tag still matches - if the SVP entry was
      // replaced after this instruction's predict, decrementing would corrupt
      // the new entry's instance counter
      if (vpq[vpq_tail].svp_hit && tag_matches(vpq[vpq_tail].svp_index, vpq[vpq_tail].pc)) {
         assert(svp[vpq[vpq_tail].svp_index].instance > 0);
         svp[vpq[vpq_tail].svp_index].instance--;
      }
   }
}


// Computes and prints SVP storage cost accounting to the stats log
// VPQ is excluded from the storage budget per spec
void vpu::print_storage(FILE *out) {
   unsigned int instance_bits = (vpq_size > 0)    ? (unsigned int)ceil(log2((double)(vpq_size + 1)))    : 1;
   unsigned int conf_bits     = (svp_conf_max > 0) ? (unsigned int)ceil(log2((double)(svp_conf_max + 1))) : 1;
   unsigned int bits_per_entry = svp_tag_bits + conf_bits + 64 + 64 + instance_bits;
   unsigned int total_bits     = svp_num_entries * bits_per_entry;
   double total_bytes          = total_bits / 8.0;
   double total_kb             = total_bytes / 1024.0;

   fprintf(out, "   One SVP entry:\n");
   fprintf(out, "      tag           : %3u bits  // num_tag_bits\n", svp_tag_bits);
   fprintf(out, "      conf          : %3u bits  // formula: (uint64_t)ceil(log2((double)(confmax+1)))\n", conf_bits);
   fprintf(out, "      retired_value :  64 bits  // RISCV64 integer size.\n");
   fprintf(out, "      stride        :  64 bits  // RISCV64 integer size. Competition opportunity: truncate stride to far fewer bits based on stride distribution of stride-predictable instructions.\n");
   fprintf(out, "      instance ctr  : %3u bits  // formula: (uint64_t)ceil(log2((double)VPQsize))\n", instance_bits);
   fprintf(out, "      -------------------------\n");
   fprintf(out, "      bits/SVP entry: %u bits\n", bits_per_entry);
   fprintf(out, "   Total storage cost (bits) = (%u SVP entries x %u bits/SVP entry) = %u bits\n", svp_num_entries, bits_per_entry, total_bits);
   fprintf(out, "   Total storage cost (bytes) = %.2f B (%.2f KB)\n", total_bytes, total_kb);
}
