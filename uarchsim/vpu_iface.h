// Project 4 - Competition
// Common interface for SVP VPU and EVES VPU
#ifndef VPU_IFACE_H
#define VPU_IFACE_H

// Include statements
#include <cstdint>
#include <cstdio>

// Instruction-type buckets passed to train()
// Mirrors predINTALU, predFPALU, predLOAD in parameters.h
enum vp_inst_type : uint8_t {
   VPT_INTALU = 0,      // Integer ALU instruction 
   VPT_FPALU  = 1,      // Floating-point ALU instruction 
   VPT_LOAD   = 2,      // Load instruction 
   VPT_COUNT  = 3       // Total number
};

// VPU Interface class
class vpu_iface {
public:
   // Virtual destructor so derived predictors clean up through the base pointer
   virtual ~vpu_iface() = default;

   // Generates a value prediction and allocates a VPQ entry
   virtual bool predict(uint64_t pc,
                        uint64_t &out_predicted_val,
                        bool &out_confident,
                        unsigned int &out_vpq_index) = 0;

   // Trains the predictor at retirement using the committed value
   virtual void train(unsigned int vpq_index,
                      uint64_t committed_val,
                      uint8_t inst_type) = 0;

   // Repairs VPQ state on squash back to the restored checkpoint
   virtual void repair(unsigned int restored_vpq_tail,
                       bool restored_vpq_tail_phase) = 0;

   // Returns the number of free VPQ entries
   virtual unsigned int vpq_free_entries() = 0;

   // VPQ head/tail accessors for checkpoint save and squash repair
   virtual unsigned int get_vpq_tail() = 0;
   virtual bool get_vpq_tail_phase() = 0;
   
   virtual unsigned int get_vpq_head() = 0;
   virtual bool get_vpq_head_phase() = 0;

   // Prints predictor storage cost accounting to the stats log
   virtual void print_storage(FILE *out) = 0;
};

#endif // VPU_IFACE_H
