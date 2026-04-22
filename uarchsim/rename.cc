#include "pipeline.h"


// Project 4 - Value Prediction
// This function checks value-prediction eligibility.
// predINTALU, predFPALU, and predLOAD are all "bool" types,
// and are configured to be true or false based on corresponding
// simulator arguments being 1 or 0, respectively.
bool pipeline_t::is_eligible(payload_t *pay) {
   // Any instruction without a destination register is ineligible.
   if (!pay->C_valid)
      return(false);          

   // If we reached this point, the instruction has a destination register.

   // instr. is INTALU type. It is eligible if predINTALU is configured "true".
   if (IS_INTALU(pay->flags))
      return(predINTALU);

   // instr. is FPALU type. It is eligible if predFPALU is configured "true".
   else if (IS_FPALU(pay->flags))
      return(predFPALU);  

   // instr. is a normal LOAD (not rare load-with-reserv). It is eligible if predLOAD is configured "true".
   else if (IS_LOAD(pay->flags) && !IS_AMO(pay->flags))
      return(predLOAD); 

   // instr. is none of the above major types, so it is never eligible  
   else
      return(false);      
}


////////////////////////////////////////////////////////////////////////////////////
// The Rename Stage has two sub-stages:
// rename1: Get the next rename bundle from the FQ.
// rename2: Rename the current rename bundle.
////////////////////////////////////////////////////////////////////////////////////

void pipeline_t::rename1() {
   unsigned int i;
   unsigned int rename1_bundle_width;

   ////////////////////////////////////////////////////////////////////////////////////
   // Try to get the next rename bundle.
   // Two conditions might prevent getting the next rename bundle, either:
   // (1) The current rename bundle is stalled in rename2.
   // (2) The FQ does not have enough instructions for a full rename bundle,
   //     and it's not because the fetch unit is stalled waiting for a
   //     serializing instruction to retire (fetch exception, amo, or csr instruction).
   ////////////////////////////////////////////////////////////////////////////////////

   // Check the first condition. Is the current rename bundle stalled, preventing
   // insertion of the next rename bundle? Check whether or not the pipeline register
   // between rename1 and rename2 still has a rename bundle.

   if (RENAME2[0].valid) { // The current rename bundle is stalled.
      return;
   }

   // Check the second condition.
   // Stall if the fetch unit is active (it's not waiting for a serializing
   // instruction to retire) and the FQ doesn't have enough instructions for a full
   // rename bundle.

   rename1_bundle_width = ((FQ.get_length() < dispatch_width) ? FQ.get_length() : dispatch_width);

   if (FetchUnit->active() && (rename1_bundle_width < dispatch_width)) {
      return;
   }

   // Get the next rename bundle.
   for (i = 0; i < rename1_bundle_width; i++) {
      assert(!RENAME2[i].valid);
      RENAME2[i].valid = true;
      RENAME2[i].index = FQ.pop();
   }
}

void pipeline_t::rename2() {
   unsigned int i;
   unsigned int index;
   unsigned int bundle_dst, bundle_branch;

   // Stall the rename2 sub-stage if either:
   // (1) There isn't a current rename bundle.
   // (2) The Dispatch Stage is stalled.
   // (3) There aren't enough rename resources for the current rename bundle.

   if (!RENAME2[0].valid || // First stall condition: There isn't a current rename bundle.
       DISPATCH[0].valid) { // Second stall condition: The Dispatch Stage is stalled.
      return;
   }

   // Third stall condition: There aren't enough rename resources for the current rename bundle.
   bundle_dst = 0;
   bundle_branch = 0;

   for (i = 0; i < dispatch_width; i++) {
      if (!RENAME2[i].valid)
         break; // Not a valid instruction: Reached the end of the rename bundle so exit loop.

      index = RENAME2[i].index;

      // FIX_ME #1
      // Count the number of instructions in the rename bundle that need a checkpoint (most branches).
      // Count the number of instructions in the rename bundle that have a destination register.
      // With these counts, you will be able to query the renamer for resource availability
      // (checkpoints and physical registers).
      //
      // Tips:
      // 1. The loop construct, for iterating through all instructions in the rename bundle (0 to dispatch_width),
      //    is already provided for you, above. Note that this comment is within the loop.
      // 2. At this point of the code, 'index' is the instruction's index into PAY.buf[] (payload).
      // 3. The instruction's payload has all the information you need to count resource needs.
      //    There is a flag in the instruction's payload that *directly* tells you if this instruction needs a checkpoint.
      //    Another field indicates whether or not the instruction has a destination register.

      // FIX_ME #1 BEGIN
      if(PAY.buf[index].checkpoint) {
         bundle_branch++;
      }

      if(PAY.buf[index].C_valid) {
         bundle_dst++;
      }
      // FIX_ME #1 END
   }

   // FIX_ME #2
   // Check if the Rename2 Stage must stall due to any of the following conditions:
   // * Not enough free checkpoints.
   // * Not enough free physical registers.
   //
   // If there are not enough resources for the *whole* rename bundle, then stall the Rename2 Stage.
   // Stalling is achieved by returning from this function ('return').
   // If there are enough resources for the *whole* rename bundle, then do not stall the Rename2 Stage.
   // This is achieved by doing nothing and proceeding to the next statements.

   // FIX_ME #2 BEGIN
   if(REN->stall_branch(bundle_branch)) {
      return;
   }

   if(REN->stall_reg(bundle_dst)) {
      return;
   }
   // FIX_ME #2 END

   // Project 4 - Value Prediction
   // VPQ stall condition: count VP-eligible instrs in the bundle,
   // stall if VPQ doesn't have enough free entries for all of them.
   if (VPU) {
      unsigned int bundle_vp_eligible = 0;
      for (unsigned int k = 0; k < dispatch_width; k++) {
         if (!RENAME2[k].valid) break;
         if (is_eligible(&PAY.buf[RENAME2[k].index]))
            bundle_vp_eligible++;
      }
      if (VPU->vpq_free_entries() < bundle_vp_eligible)
         return;
   }

   //
   // Sufficient resources are available to rename the rename bundle.
   //
   for (i = 0; i < dispatch_width; i++) {
      if (!RENAME2[i].valid)
         break; // Not a valid instruction: Reached the end of the rename bundle so exit loop.

      index = RENAME2[i].index;

      // FIX_ME #3
      // Rename source registers (first) and destination register (second).
      //
      // Tips:
      // 1. At this point of the code, 'index' is the instruction's index into PAY.buf[] (payload).
      // 2. The instruction's payload has all the information you need to rename registers, if they exist. In particular:
      //    * whether or not the instruction has a first source register, and its logical register number
      //    * whether or not the instruction has a second source register, and its logical register number
      //    * whether or not the instruction has a third source register, and its logical register number
      //    * whether or not the instruction has a destination register, and its logical register number
      // 3. When you rename a logical register to a physical register, remember to *update* the instruction's payload with the physical register specifier,
      //    so that the physical register specifier can be used in subsequent pipeline stages.

      // FIX_ME #3 BEGIN
      if(PAY.buf[index].A_valid) {
         PAY.buf[index].A_phys_reg = REN->rename_rsrc(PAY.buf[index].A_log_reg);
      }

      if(PAY.buf[index].B_valid) {
         PAY.buf[index].B_phys_reg = REN->rename_rsrc(PAY.buf[index].B_log_reg);
      }

      if(PAY.buf[index].D_valid) {
         PAY.buf[index].D_phys_reg = REN->rename_rsrc(PAY.buf[index].D_log_reg);
      }

      if(PAY.buf[index].C_valid) {
         PAY.buf[index].C_phys_reg = REN->rename_rdst(PAY.buf[index].C_log_reg);
      }
      // FIX_ME #3 END

      // FIX_ME #4
      // Get the instruction's branch mask.
      //
      // Tips:
      // 1. Every instruction gets a branch_mask. An instruction needs to know which branches it depends on, for possible squashing.
      // 2. The branch_mask is not held in the instruction's PAY.buf[] entry. Rather, it explicitly moves with the instruction
      //    from one pipeline stage to the next. Normally the branch_mask would be wires at this point in the logic but since we
      //    don't have wires place it temporarily in the RENAME2[] pipeline register alongside the instruction, until it advances
      //    to the DISPATCH[] pipeline register. The required left-hand side of the assignment statement is already provided for you below:
      //    RENAME2[i].branch_mask = ??;

      // FIX_ME #4 BEGIN
      RENAME2[i].branch_mask = REN->get_branch_mask();
      // FIX_ME #4 END

      // FIX_ME #5
      // If this instruction requires a checkpoint (most branches), then create a checkpoint.
      //
      // Tips:
      // 1. At this point of the code, 'index' is the instruction's index into PAY.buf[] (payload).
      // 2. There is a flag in the instruction's payload that *directly* tells you if this instruction needs a checkpoint.
      // 3. If you create a checkpoint, remember to *update* the instruction's payload with its branch ID
      //    so that the branch ID can be used in subsequent pipeline stages.

      // FIX_ME #5 BEGIN
      if(PAY.buf[index].checkpoint) {
         PAY.buf[index].branch_ID = REN->checkpoint();

         // Project 4: save VPQ tail (position + phase) so squash.cc can
         // repair() to this point on a branch misprediction. Indexed by
         // branch_ID. Phase is required for correctness when the VPQ wraps
         // a full vpq_size between now and the misprediction.
         if (VPU) {
            vpq_tail_chkpt[PAY.buf[index].branch_ID]       = VPU->get_vpq_tail();
            vpq_tail_chkpt_phase[PAY.buf[index].branch_ID] = VPU->get_vpq_tail_phase();
         }
      }
      // FIX_ME #5 END


      // Project 4 - Value Prediction
      // Initialize all VP payload fields to defaults
      PAY.buf[index].vp_pred      = false;
      PAY.buf[index].vp_val       = 0;
      PAY.buf[index].vp_eligible  = false;
      PAY.buf[index].vp_svp_hit   = false;
      PAY.buf[index].vp_confident = false;
      PAY.buf[index].vpq_index    = 0;

      if (is_eligible(&PAY.buf[index])) {
         PAY.buf[index].vp_eligible = true;

         if (PERFECT_VALUE_PRED) {
            // Perfect VP: use functional sim to get the exact correct value.
            // good_instruction required here (only predict on-path instrs in oracle modes).
            if (PAY.buf[index].good_instruction) {
               db_t *actual = get_pipe()->peek(PAY.buf[index].db_index);
               if (actual && actual->a_rdst[0].valid) {
                  PAY.buf[index].vp_pred = true;
                  PAY.buf[index].vp_val  = actual->a_rdst[0].value;
               }
            }
         }
         else if (VPU) {
            // Real SVP prediction
            uint64_t predicted_val;
            bool confident;
            unsigned int vpq_idx;

            bool hit = VPU->predict(PAY.buf[index].pc, predicted_val,
                                    confident, vpq_idx);

            PAY.buf[index].vpq_index  = vpq_idx;
            PAY.buf[index].vp_svp_hit = hit;

            if (hit) {
               // Always store predicted value (even unconfident) so retire can
               // check correctness for all vpmeas stat categories.
               PAY.buf[index].vp_val = predicted_val;

               if (SVP_ORACLE_CONF) {
                  // Oracle confidence: for on-path instrs, check prediction vs functional sim
                  // value and override SVP conf with that result. For off-path instrs, we have
                  // no ground truth to check, so fall back to SVP's own confidence counter.
                  // (good_instruction is allowed here, oracle mode only.)
                  if (PAY.buf[index].good_instruction) {
                     confident = false;
                     db_t *actual = get_pipe()->peek(PAY.buf[index].db_index);
                     if (actual && actual->a_rdst[0].valid)
                        confident = (predicted_val == actual->a_rdst[0].value);
                  }
                  // else: leave `confident` as the SVP's own result (conf >= conf_max)
               }
               // IMPORTANT: in real confidence mode (not oracle), we do NOT touch
               // good_instruction at all. Even wrong-path instrs get predicted.
               // Spec deducts points if good_instruction appears in the real conf path.

               PAY.buf[index].vp_confident = confident;
               if (confident) {
                  PAY.buf[index].vp_pred = true;
                  // vp_val already set above
               }
            }
         }
      }
   }

   //
   // Transfer the rename bundle from the Rename Stage to the Dispatch Stage.
   //
   for (i = 0; i < dispatch_width; i++) {
      if (!RENAME2[i].valid)
         break; // Not a valid instruction: Reached the end of the rename bundle so exit loop.

      assert(!DISPATCH[i].valid);
      RENAME2[i].valid = false;
      DISPATCH[i].valid = true;
      DISPATCH[i].index = RENAME2[i].index;
      DISPATCH[i].branch_mask = RENAME2[i].branch_mask;
   }
}
