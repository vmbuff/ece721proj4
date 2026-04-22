#include "vpu.h"
#include <cassert>
#include <cmath>

vpu_t::vpu_t(unsigned int vpq_size,
             unsigned int index_bits,
             unsigned int tag_bits,
             unsigned int conf_max) {

   this->svp_index_bits  = index_bits;
   this->svp_tag_bits    = tag_bits;
   this->svp_conf_max    = conf_max;
   this->svp_num_entries = (1U << index_bits);
   this->vpq_size        = vpq_size;

   // SVP table: all entries start invalid
   svp = new svp_entry_t[svp_num_entries];
   for (unsigned int i = 0; i < svp_num_entries; i++) {
      svp[i].tag           = 0;
      svp[i].stride        = 0;
      svp[i].retired_value = 0;
      svp[i].instance      = 0;
      svp[i].conf          = 0;
      svp[i].valid         = false;
   }

   // VPQ: empty (head == tail, same phase)
   vpq = new vpq_entry_t[vpq_size];
   vpq_head       = 0;
   vpq_head_phase = false;
   vpq_tail       = 0;
   vpq_tail_phase = false;
}

vpu_t::~vpu_t() {
   delete[] svp;
   delete[] vpq;
}


//
// Helpers
//

// PC layout: [63 ... | tag | index | 0]
// Bit 0 is always 0 (RISC-V alignment), discard it.
unsigned int vpu_t::get_svp_index(uint64_t pc) {
   return (unsigned int)((pc >> 1) & ((1U << svp_index_bits) - 1));
}

uint64_t vpu_t::get_svp_tag(uint64_t pc) {
   if (svp_tag_bits == 0) return 0;
   return (pc >> (1 + svp_index_bits)) & ((1ULL << svp_tag_bits) - 1);
}

bool vpu_t::tag_matches(unsigned int idx, uint64_t pc) {
   if (svp_tag_bits == 0) return true;       // no tags = always matches
   if (!svp[idx].valid) return false;
   return (svp[idx].tag == get_svp_tag(pc));
}

// Walk VPQ head to tail counting entries that will actually contribute a
// future decrement to svp[svp_index].instance. An entry qualifies iff:
//   (a) svp_hit=true at predict time (the entry actually did instance++), AND
//   (b) its PC tag matches the CURRENT svp[svp_index].tag (the entry will
//       hit the tag-match decrement path at train, not the tag-miss replace).
// Counting entries that fail (a) or (b) creates "phantom" instance increments
// that are never decremented -- causing instance drift, confidently wrong
// predictions whose stride remains correct, and persistent conf_incorr.
uint64_t vpu_t::count_inflight_instances(unsigned int svp_index) {
   uint64_t count = 0;
   unsigned int pos = vpq_head;
   bool phase = vpq_head_phase;

   while (!(pos == vpq_tail && phase == vpq_tail_phase)) {
      if (vpq[pos].svp_index == svp_index &&
          vpq[pos].svp_hit &&
          tag_matches(svp_index, vpq[pos].pc))
         count++;
      pos++;
      if (pos == vpq_size) { pos = 0; phase = !phase; }
   }
   return count;
}


//
// VPQ free entry count (phase-bit logic, same pattern as renamer FL/AL)
//

unsigned int vpu_t::vpq_free_entries() {
   if (vpq_head == vpq_tail) {
      // Same position: same phase = empty (all free), different phase = full (none free)
      return (vpq_head_phase == vpq_tail_phase) ? vpq_size : 0;
   }
   else if (vpq_tail > vpq_head) {
      return vpq_size - (vpq_tail - vpq_head);
   }
   else {
      return vpq_head - vpq_tail;
   }
}

unsigned int vpu_t::get_vpq_tail() {
   return vpq_tail;
}

unsigned int vpu_t::get_vpq_head() {
   return vpq_head;
}

bool vpu_t::get_vpq_tail_phase() {
   return vpq_tail_phase;
}

bool vpu_t::get_vpq_head_phase() {
   return vpq_head_phase;
}


//
// predict(): called from rename2() per VP-eligible instruction
//

bool vpu_t::predict(uint64_t pc,
                    uint64_t &out_predicted_val,
                    bool &out_confident,
                    unsigned int &out_vpq_index) {

   unsigned int idx = get_svp_index(pc);
   bool hit = (svp[idx].valid && tag_matches(idx, pc));

   if (hit) {
      // Increment BEFORE computing prediction so this in-flight instance
      // predicts the next value (retired_value + stride), not the last-committed
      // value (retired_value + 0).
      svp[idx].instance++;

      out_predicted_val = svp[idx].retired_value +
                          (uint64_t)((int64_t)svp[idx].instance * svp[idx].stride);
      out_confident = (svp[idx].conf >= svp_conf_max);
   }

   // Always allocate a VPQ entry, even on miss.
   // Misses still need training at retire (they become replacements in the SVP),
   // and repair() needs to walk all entries on squash.
   assert(vpq_free_entries() > 0);
   out_vpq_index = vpq_tail;

   vpq[vpq_tail].pc              = pc;
   vpq[vpq_tail].svp_index       = idx;
   vpq[vpq_tail].predicted_value = hit ? out_predicted_val : 0;
   vpq[vpq_tail].svp_hit         = hit;
   vpq[vpq_tail].confident       = hit ? out_confident : false;

   // Advance tail
   vpq_tail++;
   if (vpq_tail == vpq_size) { vpq_tail = 0; vpq_tail_phase = !vpq_tail_phase; }

   return hit;
}


//
// train(): called from retire.cc per VP-eligible retired instruction
//

void vpu_t::train(unsigned int vpq_index, uint64_t committed_val) {
   // Should be training the head entry (in-order retirement)
   assert(vpq_index == vpq_head);

   unsigned int idx = vpq[vpq_index].svp_index;
   uint64_t pc = vpq[vpq_index].pc;

   if (svp[idx].valid && tag_matches(idx, pc)) {
      // Tag match: only update state if this VPQ entry actually predicted
      // against the current SVP entry. If svp_hit was false at predict
      // (svp was replaced and then replaced back to a tag matching this
      // entry's PC), the entry's committed value has no meaningful
      // relationship to the current entry's retired_value/stride/conf.
      // Touching them would pollute a stable hot entry with a spurious
      // stride, resetting conf. Skip the entire update in that case.
      if (vpq[vpq_index].svp_hit) {
         int64_t new_stride = (int64_t)(committed_val - svp[idx].retired_value);

         if (new_stride == svp[idx].stride) {
            if (svp[idx].conf < svp_conf_max)
               svp[idx].conf++;
         }
         else {
            svp[idx].stride = new_stride;
            svp[idx].conf   = 0;
         }

         svp[idx].retired_value = committed_val;

         assert(svp[idx].instance > 0);
         svp[idx].instance--;
      }
   }
   else {
      // Tag miss or invalid. Only replace if the slot is unoccupied
      // (not valid) or the current entry has lost confidence (conf == 0).
      // A confident hot entry should survive aliasing collisions --
      // unconditional replacement causes severe thrashing when many PCs
      // alias on the same svp_index (VAL-7: 128 entries, 10-bit tags).
      if (!svp[idx].valid || svp[idx].conf == 0) {
         svp[idx].tag           = get_svp_tag(pc);
         svp[idx].retired_value = committed_val;
         svp[idx].stride        = (int64_t)committed_val;  // spec: "retired_value = stride = value"
         svp[idx].conf          = 0;
         svp[idx].valid         = true;

         // Instance = how many other in-flight instrs map to this index
         // AND satisfy the instance invariant (svp_hit=T, tag match).
         unsigned int save_head = vpq_head;
         bool save_phase = vpq_head_phase;
         vpq_head++;
         if (vpq_head == vpq_size) { vpq_head = 0; vpq_head_phase = !vpq_head_phase; }
         svp[idx].instance = count_inflight_instances(idx);
         vpq_head = save_head;
         vpq_head_phase = save_phase;
      }
      // else: confident entry kept, this retire's data is dropped.
      // No svp mutation, no instance change.
   }

   // Free VPQ head
   vpq_head++;
   if (vpq_head == vpq_size) { vpq_head = 0; vpq_head_phase = !vpq_head_phase; }
}


//
// repair(): called from squash.cc on any pipeline squash
//

void vpu_t::repair(unsigned int restored_vpq_tail, bool restored_vpq_tail_phase) {
   // Walk backward from current (tail, tail_phase) to (restored, restored_phase),
   // undoing speculative instance increments for each discarded hit entry.
   // Comparing both position AND phase is required: in long branch-resolution
   // windows the VPQ can wrap a full vpq_size back to the same position with
   // phase flipped; position-only would make this a no-op.
   while (vpq_tail != restored_vpq_tail || vpq_tail_phase != restored_vpq_tail_phase) {
      // Step backward
      if (vpq_tail == 0) { vpq_tail = vpq_size - 1; vpq_tail_phase = !vpq_tail_phase; }
      else                 vpq_tail--;

      // Undo the instance increment that predict() did for this entry.
      // Only decrement if the entry's PC tag STILL matches the current
      // svp tag -- if svp[idx] has been replaced since this entry's
      // predict, the ++ went to a tag that no longer exists; decrementing
      // the new tag's instance is erroneous (it makes the new tag's
      // instance negative / under-counts).
      if (vpq[vpq_tail].svp_hit &&
          tag_matches(vpq[vpq_tail].svp_index, vpq[vpq_tail].pc)) {
         assert(svp[vpq[vpq_tail].svp_index].instance > 0);
         svp[vpq[vpq_tail].svp_index].instance--;
      }
   }
}


//
// print_storage(): SVP cost accounting (VPQ excluded per spec)
//

void vpu_t::print_storage(FILE *out) {
   unsigned int instance_bits = (vpq_size > 0) ? (unsigned int)ceil(log2((double)(vpq_size + 1))) : 1;
   unsigned int conf_bits = (svp_conf_max > 0) ? (unsigned int)ceil(log2((double)(svp_conf_max + 1))) : 1;
   unsigned int bits_per_entry = svp_tag_bits + conf_bits + 64 + 64 + instance_bits;
   unsigned int total_bits = svp_num_entries * bits_per_entry;
   double total_bytes = total_bits / 8.0;
   double total_kb = total_bytes / 1024.0;

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
