// Include statements
#include <inttypes.h>
#include "renamer.h"

using std::vector;


// Renamer Class Functions
// Functions related to Rename Stage:

// This function stalls rename stage if there aren't enough free physical 
// registers
bool renamer::stall_reg(uint64_t bundle_dst) {
	// If the number of logical destination registers in the current rename bundle 
	// is greater than the number of free physical registers
	if(bundle_dst > FL_count) {	
		return true;		// Stall
	} else {
		return false;		// Don't stall
	}
}

// This function stalls rename stage if there aren't enough free checkpoints 
// for all branches
bool renamer::stall_branch(uint64_t bundle_branch) {
	// If the number of branches in the current rename bundle is greater than the 
	// number of free checkpoints for branches
	if(bundle_branch > (n_branches - checkpoint_count)) {
		return true;		// Stall
	} else {
		return false;		// Don't stall
	}
}

// This function is used to get the branch mask for an instruction
uint64_t renamer::get_branch_mask() {
	// Return the current value of the GBM as the branch mask for the instruction
	return GBM;						
}

// This function is used to rename a single source register 
uint64_t renamer::rename_rsrc(uint64_t log_reg) {
	// Return the physical register mapping for the logical register
	return RMT[log_reg].phys_reg;		
}

// This function is used to rename a single destination register
uint64_t renamer::rename_rdst(uint64_t log_reg) {
	// Get a new register from the Free List
	uint64_t new_phys_reg = FL[FL_head].phys_reg;

	// Update the Free List head pointer and its phase bit
	FL_head++;								// Increment head pointer
	if(FL_head == FL.size()) {				// If head pointer reaches the end of the Free List
		FL_head = 0;						// Wrap around head pointer
		FL_head_phase = !FL_head_phase;		// Toggle head pointer phase bit
	}

	// Update the Rename Map Table with the new physical register mapping for the logical register
	RMT[log_reg].phys_reg = new_phys_reg;

	// Set the ready bit of the new physical register to false
	PRF_ready[new_phys_reg].ready = false;		

	// Update the Free List register count
	FL_count--;								// Decrement count of free registers

	// Return the new physical register mapping for the logical register
	return new_phys_reg;
}

// This function creates a new branch checkpoint
uint64_t renamer::checkpoint() {
	uint64_t branch_ID = 0;				// Local variable to hold the branch ID for the new branch

	// Iterate through GBM to find a free checkpoint bit (branch ID) for the new branch
	for(uint64_t i = 0; i < n_branches; i++) {
		// If the bit at position i in the GBM is '0', it is free and can be allocated to the new branch
		if((GBM & (1ULL << i)) == 0ULL) {
			GBM |= (1ULL << i);			// Set the bit at position i in the GBM to '1' to indicate it is now in use
			branch_ID = i;				// The branch ID for the new branch is the position of the allocated bit in the GBM
			checkpoint_count++;			// Increment the checkpoint count
			break;						// Break out of the loop
		}
	}

	// Save the checkpoint contents
	checkpoints[branch_ID].shadow_RMT = RMT;				// Save the current Rename Map Table 
	checkpoints[branch_ID].FL_head = FL_head;				// Save the current Free List head pointer
	checkpoints[branch_ID].FL_head_phase = FL_head_phase;	// Save the current Free List head pointer phase bit
	checkpoints[branch_ID].GBM = GBM;						// Save the current GBM

	// Return the branch ID for the new branch
	return branch_ID;
}


// Functions related to Dispatch Stage:

// This function stalls dispatch stage if there aren't enough free entries 
// in the Active List
bool renamer::stall_dispatch(uint64_t bundle_inst) {
	// If the number of instructions in the current dispatch bundle is greater than 
	// the number of free entries in the Active List
	if(bundle_inst > (n_active - AL_count)) {
		return true;		// Stall
	} else {
		return false;		// Don't stall
	}
}

// This function dispatches a single instruction into the Active List
uint64_t renamer::dispatch_inst(bool dest_valid, uint64_t log_reg, 
							    uint64_t phys_reg, bool load, bool store, 
								bool branch, bool amo, bool csr, uint64_t PC) {
	uint64_t AL_index = AL_tail;			// Local variable to store current index of the Active List

	// Create a new entry in the Active List for the dispatched instruction
	AL[AL_index].dest_valid = dest_valid;	// Set destination valid flag
	AL[AL_index].log_reg = log_reg;			// Set logical register number
	AL[AL_index].phys_reg = phys_reg;		// Set physical register number

	AL[AL_index].completed = false;			// Set completed bit to false
	AL[AL_index].exception = false;			// Set exception bit to false
	AL[AL_index].load_viol = false;			// Set load violation bit to false
	AL[AL_index].br_misp = false;  			// Set branch misprediction bit to false
	AL[AL_index].val_misp = false;			// Set value misprediction bit to false

	AL[AL_index].load = load;				// Set load flag
	AL[AL_index].store = store;				// Set store flag
	AL[AL_index].branch = branch;			// Set branch flag
	AL[AL_index].amo = amo;					// Set atomic memory operation flag
	AL[AL_index].csr = csr;					// Set system instruction flag

	AL[AL_index].PC = PC;					// Set program counter

	// Update the Active List tail pointer and its phase bit
	AL_tail++;								// Increment tail pointer
	if(AL_tail == AL.size()) {				// If tail pointer reaches the end of the Active List
		AL_tail = 0;						// Wrap around tail pointer
		AL_tail_phase = !AL_tail_phase;		// Toggle tail pointer phase bit
	}

	// Update the Active List register count
	AL_count++;								// Increment number of entries in Active List

	// Return the index of the dispatched instruction in the Active List
	return AL_index;
}

// This function tests the ready bit of the indicated physical register
bool renamer::is_ready(uint64_t phys_reg) {
	// Return the ready bit of the indicated physical register
	return PRF_ready[phys_reg].ready;			
}

// This function clears the ready bit of the indicated physical register
void renamer::clear_ready(uint64_t phys_reg) {
	// Clear the ready bit of the indicated physical register
	PRF_ready[phys_reg].ready = false;	
}


// Functions related to the Reg. Read and Execute Stages:

// This function returns the contents (value) of the indicated physical register
uint64_t renamer::read(uint64_t phys_reg) {
	// Return the value of the indicated physical register
	return PRF[phys_reg].value;
}

// This function sets the ready bit of the indicated physical register
void renamer::set_ready(uint64_t phys_reg) {
	// Set the ready bit of the indicated physical register
	PRF_ready[phys_reg].ready = true;
}


// Functions related to Writeback Stage:

// This function writes a value into the indicated physical register
void renamer::write(uint64_t phys_reg, uint64_t value) {
	// Write the value into the indicated physical register
	PRF[phys_reg].value = value;
}

// This function sets the completed bit of the indicated entry in the Active List
void renamer::set_complete(uint64_t AL_index) {
	// Set the completed bit of the indicated entry in the Active List to true
	AL[AL_index].completed = true;
}

// This function is for handling branch resolution
void renamer::resolve(uint64_t AL_index, uint64_t branch_ID, bool correct) {
	// If the branch was mispredicted and recovery is required
	if(correct == false) {
		// Restore processor back to the state corresponding to the branch's checkpoint
		RMT = checkpoints[branch_ID].shadow_RMT;				// Restore the Rename Map Table
		FL_head = checkpoints[branch_ID].FL_head;				// Restore the Free List head pointer
		FL_head_phase = checkpoints[branch_ID].FL_head_phase;	// Restore the Free List head pointer phase bit
		GBM = checkpoints[branch_ID].GBM;						// Restore the GBM

		// Clear the branch's bit in the restored GBM to indicate it is now free
		GBM &= ~(1ULL << branch_ID);

		// Restore the Active List 
		// Update the Active List count
		if(AL_index >= AL_head) {
			// If the tail pointer is ahead of the head pointer in the Active List (not wrapped around)
			AL_count = (AL_index - AL_head) + 1;					
		} else {
			// If the tail pointer is behind the head pointer in the Active List (wrapped around)
			AL_count = ((AL.size() - AL_head) + AL_index) + 1;
		}

		// Update the Active List tail pointer and its phase bit
		AL_tail = AL_head;					// Reset tail to head 
		AL_tail_phase = AL_head_phase;		// Reset tail phase bit to head phase bit

		// Iterate through the Active List
		for(uint64_t i = 0; i < AL_count; i++) {
			AL_tail++;								// Increment tail pointer
			// If tail pointer reaches the end of the Active List, wrap around 
			if(AL_tail == AL.size()) {
				AL_tail = 0;						// Wrap around tail pointer
				AL_tail_phase = !AL_tail_phase;		// Toggle tail pointer phase bit
			}
		}

		// Restore the checkpoint count
		checkpoint_count = 0;				// Reset checkpoint count to 0
		for(uint64_t i = 0; i < n_branches; i++) {
			if((GBM & (1ULL << i))) {		// If the bit at position i in the GBM is '1', it is in use
				checkpoint_count++;			// Increment checkpoint count
			}
		}

		// Restore the Free List
		// If head and tail pointers are the same
		if(FL_head == FL_tail) {
			// If the head pointer's phase bit and the tail pointer's phase bit are the same
			if(FL_head_phase == FL_tail_phase) {
				FL_count = 0;				// Empty
			} else {
				FL_count = FL.size();		// Full
			}
		// If head and tail pointers are different
		} else if (FL_head_phase == FL_tail_phase) {
			if(FL_tail >= FL_head) {
				// If the tail pointer is ahead of the head pointer in the Free List (not wrapped around)
				FL_count = FL_tail - FL_head;	
			} else {
				// If the tail pointer is behind the head pointer in the Free List (wrapped around)
				FL_count = (FL.size() - FL_head) + FL_tail;
			}
		// If head and tail pointers are different and their phase bits are different
		} else {
			FL_count = FL.size() - FL_head + FL_tail;
		}
		
	// If the branch was correctly predicted and recovery is not required
	} else {
		// Clear the branch's bit in the GBM to indicate it is now free
		GBM &= ~(1ULL << branch_ID);		

		// Clear the branch's bit in all checkpointed GBMs to indicate it is now free
		for(uint64_t i = 0; i < n_branches; i++) {
			checkpoints[i].GBM &= ~(1ULL << branch_ID);
		}

		// Update the checkpoint count
		checkpoint_count--;					// Decrement checkpoint count
	}
}


// Functions related to Retire Stage:

// This function allows the caller to examine the instruction at the head
// of the Active List
bool renamer::precommit(bool &completed, bool &exception, bool &load_viol, bool &br_misp, 
						bool &val_misp, bool &load, bool &store, bool &branch, bool &amo, 
						bool &csr, uint64_t &PC) {
	// If the Active List is not empty
	if(AL_count > 0) {
		// Return the contents of the head entry of the Active List
		completed = AL[AL_head].completed;		// Set completed bit

		exception = AL[AL_head].exception;		// Set exception bit
		load_viol = AL[AL_head].load_viol;		// Set load violation bit
		br_misp = AL[AL_head].br_misp;			// Set branch misprediction bit
		val_misp = AL[AL_head].val_misp;		// Set value misprediction bit

		load = AL[AL_head].load;				// Set load flag
		store = AL[AL_head].store;				// Set store flag
		branch = AL[AL_head].branch;			// Set branch flag
		amo = AL[AL_head].amo;					// Set atomic memory operation flag
		csr = AL[AL_head].csr;					// Set system instruction flag

		PC = AL[AL_head].PC;					// Set program counter

		return true;							// Active List is not empty
	// If the Active List is empty
	} else {
		return false;							// Active List is empty
	}
}

// This function commits the instruction at the head of the Active List
void renamer::commit() {
	// If the instruction has a valid destination register
	if(AL[AL_head].dest_valid) {
		// Local variables to hold old and new physical register mappings
		uint64_t old_phys_reg = AMT[AL[AL_head].log_reg].phys_reg;		// Old physical register mapping
		uint64_t new_phys_reg = AL[AL_head].phys_reg;					// New physical register mapping

		// Update the AMT with the new physical register mapping
		AMT[AL[AL_head].log_reg].phys_reg = new_phys_reg;

		// Add the old physical register mapping to the Free List
		FL[FL_tail].phys_reg = old_phys_reg;		

		// Update the Free List tail pointer and its phase bit
		FL_tail++;								// Increment tail pointer
		if(FL_tail == FL.size()) {				// If tail pointer reaches the end of the Free List
			FL_tail = 0;						// Wrap around tail pointer
			FL_tail_phase = !FL_tail_phase;		// Toggle tail pointer phase bit
		}

		// Update the Free List register count
		FL_count++;								// Increment count of free registers	
	}

	// Update the Active List head pointer and its phase bit
	AL_head++;								// Increment head pointer
	if(AL_head == AL.size()) {				// If head pointer reaches the end of the Active List
		AL_head = 0;						// Wrap around head pointer
		AL_head_phase = !AL_head_phase;		// Toggle head pointer phase bit
	}

	// Update the Active List entry count
	AL_count--;								// Decrement count of entries in Active List
}

// This function performs a squash of the pipeline
void renamer::squash() {
	// Restore the RMT to the AMT
	for(uint64_t i = 0; i < n_log_regs; i++) {
		RMT[i].phys_reg = AMT[i].phys_reg;
	}

	// Clear the GBM and checkpoint counter to indicate all checkpoints are now free
	GBM = 0;					// Clear GBM
	checkpoint_count = 0;		// Reset checkpoint count to 0

	// Update the Active List
	AL_head = 0;				// Reset head pointer to 0
	AL_head_phase = 0;			// Reset head pointer phase bit to 0
	AL_tail = 0;				// Reset tail pointer to 0
	AL_tail_phase = 0;			// Reset tail pointer phase bit to 0
	AL_count = 0;				// Reset count of entries in Active List to 0

	// Update the Free List
	uint64_t FL_index = 0;		// Local variable to hold current index for iterating through Free List

	// Iterate through the Free List and add all physical registers back to the Free List
	for(uint64_t i = 0; i < n_phys_regs; i++) {
		// Flag to indicate whether the physical register is currently mapped in the AMT
		bool in_AMT = false;	

		// Iterate through the AMT to check if the physical register is currently mapped in the AMT
		for(uint64_t j = 0; j < n_log_regs; j++) {
			// If the physical register is currently mapped in the AMT
			if(AMT[j].phys_reg == i) {		
				in_AMT = true;				// Set flag to true
				break;						// Break out of inner loop
			}
		}

		// If the physical register is not currently mapped in the AMT
		if(in_AMT == false) {
			FL[FL_index].phys_reg = i;		// Add it back to the Free List
			FL_index++;						// Increment Free List index
		}
	}

	FL_count = FL_index;		// Update count of free registers in Free List

	FL_head = 0;				// Reset Free List head pointer to 0
	FL_head_phase = false;		// Reset Free List head pointer phase bit to 0
	FL_tail = 0;				// Reset Free List tail pointer to 0
	FL_tail_phase = true;		// Reset Free List tail pointer phase bit to 1

	// Update ready bits of all physical registers in the PRF to true (indicating they are ready)
	for(uint64_t i = 0; i < n_phys_regs; i++) {
		PRF_ready[i].ready = true;
	}
}


// Functions not tied to specific stage:

// Functions for setting the exception bit of the indicated entry in the Active List
void renamer::set_exception(uint64_t AL_index) {
	// Set the exception bit to true
	AL[AL_index].exception = true;
}

// Function for setting the load violation bit of the indicated entry in the Active List
void renamer::set_load_violation(uint64_t AL_index) {
	// Set the load violation bit to true
	AL[AL_index].load_viol = true;
}

// Function for setting the branch misprediction bit of the indicated entry in the Active List
void renamer::set_branch_misprediction(uint64_t AL_index) {
	// Set the branch misprediction bit to true
	AL[AL_index].br_misp = true;
}

// Function for setting the value misprediction bit of the indicated entry in the Active List
void renamer::set_value_misprediction(uint64_t AL_index) {
	// Set the value misprediction bit to true
	AL[AL_index].val_misp = true;
}

// Function to query the exception bit of the indicated entry in 
// the Active List
bool renamer::get_exception(uint64_t AL_index) {
	// Return the value of the exception bit
	return AL[AL_index].exception;
}


// Constructor
renamer::renamer(uint64_t n_log_regs, uint64_t n_phys_regs, uint64_t n_branches, uint64_t n_active) : 
	n_log_regs(n_log_regs), n_phys_regs(n_phys_regs), n_branches(n_branches), n_active(n_active) {
	// Resize structures
	RMT.resize(n_log_regs);								// Resize Rename Map Table
	AMT.resize(n_log_regs);								// Resize Architectural Map Table

	FL.resize(n_phys_regs - n_log_regs);				// Resize Free List
	AL.resize(n_active);								// Resize Active List

	PRF.resize(n_phys_regs);							// Resize Physical Register File
	PRF_ready.resize(n_phys_regs);						// Resize Physical Register File Ready Bit Table

	checkpoints.resize(n_branches);						// Resize Checkpoints

	for(uint64_t i = 0; i < n_branches; i++) {
		checkpoints[i].shadow_RMT.resize(n_log_regs);	// Resize Shadow Rename Map Table in each checkpoint
	}

	// Initialize structures
	for(uint64_t i = 0; i < n_log_regs; i++) {
		RMT[i].phys_reg = i;							// Initialize Rename Map Table 
		AMT[i].phys_reg = i;							// Initialize Architectural Map Table
	}

	for(uint64_t i = 0; i < FL.size(); i++) {
		FL[i].phys_reg = n_log_regs + i;				// Initialize Free List
	}

	FL_count = FL.size();			// Initialize count of free registers in Free List
	FL_head = 0;					// Initialize Free List head pointer to 0
	FL_head_phase = false;			// Initialize Free List head pointer phase bit to 0
	FL_tail = 0;					// Initialize Free List tail pointer to 0
	FL_tail_phase = true;			// Initialize Free List tail pointer phase bit to 1

	AL_count = 0;					// Initialize count of entries in Active List to 0
	AL_head = 0;					// Initialize Active List head pointer to 0
	AL_head_phase = false;			// Initialize Active List head pointer phase bit to 0
	AL_tail = 0;					// Initialize Active List tail pointer to 0
	AL_tail_phase = false;			// Initialize Active List tail pointer phase bit to 0

	GBM = 0;						// Initialize GBM to 0
	checkpoint_count = 0;			// Initialize checkpoint count to 0

	for(uint64_t i = 0; i < n_phys_regs; i++) {
		PRF[i].value = 0;						// Initialize Physical Register File values to 0
		PRF_ready[i].ready = true;				// Initialize Physical Register File ready bits to true
	}

	for(uint64_t i = 0; i < n_branches; i++) {
		checkpoints[i].FL_head = 0;				// Initialize Free List head pointer in each checkpoint to 0
		checkpoints[i].FL_head_phase = false;	// Initialize Free List head pointer phase bit in each checkpoint to 0
		checkpoints[i].GBM = 0;					// Initialize GBM in each checkpoint to 0
	}
}
