/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */
#include "chrome9_ps_schedule.h"

#include <limits.h>
#include <string.h>

#define CHROME9_PS_SCHEDULE_WORDS \
   ((CHROME9_PS_MAX_INSTRUCTIONS + 63) / 64)
#define CHROME9_PS_SCHEDULE_TARGET 12
/* Hard bound for the fallback's O(variants * instruction_count^2) work. */
#define CHROME9_PS_SCHEDULE_VARIANTS 8192

struct chrome9_ps_schedule_node {
   uint64_t dependency[CHROME9_PS_SCHEDULE_WORDS];
   unsigned height;
};

static unsigned
source_count(enum chrome9_ps_opcode op)
{
   switch (op) {
   case CHROME9_PS_OPCODE_TEX:
   case CHROME9_PS_OPCODE_MOV:
   case CHROME9_PS_OPCODE_EXP2:
   case CHROME9_PS_OPCODE_LOG2:
   case CHROME9_PS_OPCODE_RCP:
   case CHROME9_PS_OPCODE_RSQ:
   case CHROME9_PS_OPCODE_ABS:
   case CHROME9_PS_OPCODE_FRC:
   case CHROME9_PS_OPCODE_KILL:
      return 1;
   case CHROME9_PS_OPCODE_MUL:
   case CHROME9_PS_OPCODE_ADD:
   case CHROME9_PS_OPCODE_SUB:
   case CHROME9_PS_OPCODE_MIN:
   case CHROME9_PS_OPCODE_MAX:
   case CHROME9_PS_OPCODE_DP3:
   case CHROME9_PS_OPCODE_DP4:
      return 2;
   case CHROME9_PS_OPCODE_MAD:
   case CHROME9_PS_OPCODE_CMP:
      return 3;
   default:
      return 0;
   }
}

static void
set_bit(uint64_t bits[CHROME9_PS_SCHEDULE_WORDS], unsigned bit)
{
   bits[bit / 64] |= 1ull << (bit % 64);
}

static bool
test_bit(const uint64_t bits[CHROME9_PS_SCHEDULE_WORDS], unsigned bit)
{
   return bits[bit / 64] & (1ull << (bit % 64));
}

static bool
dependencies_done(const struct chrome9_ps_schedule_node *node,
                  const uint64_t done[CHROME9_PS_SCHEDULE_WORDS])
{
   for (unsigned w = 0; w < CHROME9_PS_SCHEDULE_WORDS; w++)
      if (node->dependency[w] & ~done[w])
         return false;
   return true;
}

static unsigned
instruction_temp_uses(const struct chrome9_ps_instruction *instruction,
                      unsigned temp)
{
   unsigned count = 0;

   for (unsigned s = 0; s < source_count(instruction->op); s++)
      if (instruction->source[s].file == CHROME9_PS_SOURCE_TEMP &&
          instruction->source[s].id == temp)
         count++;
   return count;
}

bool
chrome9_ps_ir_temp_pressure(const struct chrome9_ps_ir *ir,
                            unsigned *peak_live)
{
   unsigned last_use[CHROME9_PS_MAX_VIRTUAL_TEMPS];
   bool allocated[CHROME9_PS_MAX_VIRTUAL_TEMPS] = { 0 };
   bool live[CHROME9_PS_MAX_VIRTUAL_TEMPS] = { 0 };
   unsigned live_count = 0;
   unsigned peak = 0;

   if (!ir || !peak_live || !ir->count ||
       ir->count > CHROME9_PS_MAX_INSTRUCTIONS)
      return false;
   for (unsigned temp = 0; temp < CHROME9_PS_MAX_VIRTUAL_TEMPS; temp++)
      last_use[temp] = UINT_MAX;
   for (unsigned i = 0; i < ir->count; i++) {
      const struct chrome9_ps_instruction *instruction = &ir->instruction[i];
      unsigned sources = source_count(instruction->op);

      if (!sources)
         return false;
      for (unsigned s = 0; s < sources; s++) {
         const struct chrome9_ps_source *source = &instruction->source[s];

         if (source->file != CHROME9_PS_SOURCE_TEMP)
            continue;
         if (source->id >= CHROME9_PS_MAX_VIRTUAL_TEMPS)
            return false;
         last_use[source->id] = i;
      }
   }
   for (unsigned i = 0; i < ir->count; i++) {
      const struct chrome9_ps_instruction *instruction = &ir->instruction[i];

      if (instruction->destination.file == CHROME9_PS_DESTINATION_TEMP) {
         unsigned temp = instruction->destination.id;

         if (temp >= CHROME9_PS_MAX_VIRTUAL_TEMPS)
            return false;
         if (!allocated[temp]) {
            allocated[temp] = true;
            live[temp] = true;
            live_count++;
            if (peak < live_count)
               peak = live_count;
         }
      }
      for (unsigned s = 0; s < source_count(instruction->op); s++) {
         const struct chrome9_ps_source *source = &instruction->source[s];

         if (source->file == CHROME9_PS_SOURCE_TEMP && live[source->id] &&
             last_use[source->id] == i) {
            live[source->id] = false;
            live_count--;
         }
      }
   }
   *peak_live = peak;
   return true;
}

static bool
chrome9_schedule_ps_attempt(
   const struct chrome9_ps_ir *original,
   const struct chrome9_ps_schedule_node node[CHROME9_PS_MAX_INSTRUCTIONS],
   const unsigned total_uses[CHROME9_PS_MAX_VIRTUAL_TEMPS],
   struct chrome9_ps_ir *output, unsigned *peak_live, unsigned variant)
{
   uint64_t done[CHROME9_PS_SCHEDULE_WORDS] = { 0 };
   unsigned remaining_uses[CHROME9_PS_MAX_VIRTUAL_TEMPS];
   bool live[CHROME9_PS_MAX_VIRTUAL_TEMPS] = { 0 };
   unsigned live_count = 0;
   unsigned peak = 0;
   uint32_t diversity = variant * 0x9e3779b9u + 1;

   memcpy(remaining_uses, total_uses, sizeof(remaining_uses));
   memset(output, 0, sizeof(*output));
   for (unsigned out = 0; out < original->count; out++) {
      unsigned best = UINT_MAX;
      int best_delta = INT_MIN;
      int best_score = INT_MIN;
      unsigned best_follow = 0;
      unsigned best_height = 0;

      for (unsigned i = 0; i < original->count; i++) {
         const struct chrome9_ps_instruction *instruction;
         bool seen[CHROME9_PS_MAX_VIRTUAL_TEMPS] = { 0 };
         unsigned killed = 0;
         unsigned follow = 0;
         unsigned birth = 0;

         if (test_bit(done, i) || !dependencies_done(&node[i], done))
            continue;
         instruction = &original->instruction[i];
         if (instruction->destination.file == CHROME9_PS_DESTINATION_TEMP &&
             remaining_uses[instruction->destination.id] &&
             !live[instruction->destination.id])
            birth = 1;
         for (unsigned s = 0; s < source_count(instruction->op); s++) {
            const struct chrome9_ps_source *source = &instruction->source[s];
            unsigned uses;

            if (source->file != CHROME9_PS_SOURCE_TEMP || seen[source->id])
               continue;
            seen[source->id] = true;
            uses = instruction_temp_uses(instruction, source->id);
            if (remaining_uses[source->id] == uses)
               killed++;
            if (live[source->id])
               follow++;
         }

         int delta = (int)killed - (int)birth;

         if (!variant) {
            if (best == UINT_MAX || delta > best_delta ||
                (delta == best_delta && follow > best_follow) ||
                (delta == best_delta && follow == best_follow &&
                 node[i].height > best_height) ||
                (delta == best_delta && follow == best_follow &&
                 node[i].height == best_height && i < best)) {
               best = i;
               best_delta = delta;
               best_follow = follow;
               best_height = node[i].height;
            }
         } else {
            /* Deterministic bounded retries diversify legal ready-node
             * choices only after the fast critical-path greedy pass misses
             * the twelve-register hardware envelope. */
            diversity ^= diversity << 13;
            diversity ^= diversity >> 17;
            diversity ^= diversity << 5;
            int score = 20 * delta + 5 * (int)follow +
                        (int)(diversity % 19);

            if (best == UINT_MAX || score > best_score ||
                (score == best_score && i < best)) {
               best = i;
               best_score = score;
            }
         }
      }
      if (best == UINT_MAX)
         return false;

      const struct chrome9_ps_instruction *instruction =
         &original->instruction[best];

      output->instruction[out] = *instruction;
      set_bit(done, best);
      if (instruction->destination.file == CHROME9_PS_DESTINATION_TEMP &&
          remaining_uses[instruction->destination.id] &&
          !live[instruction->destination.id]) {
         live[instruction->destination.id] = true;
         live_count++;
         if (peak < live_count)
            peak = live_count;
      }
      for (unsigned s = 0; s < source_count(instruction->op); s++) {
         const struct chrome9_ps_source *source = &instruction->source[s];

         if (source->file != CHROME9_PS_SOURCE_TEMP)
            continue;
         if (!remaining_uses[source->id])
            return false;
         remaining_uses[source->id]--;
         if (!remaining_uses[source->id] && live[source->id]) {
            live[source->id] = false;
            live_count--;
         }
      }
   }
   output->count = original->count;
   if (live_count)
      return false;
   *peak_live = peak;
   return true;
}

bool
chrome9_schedule_ps_ir(const struct chrome9_ps_ir *input,
                       struct chrome9_ps_ir *output,
                       unsigned *peak_live)
{
   struct chrome9_ps_schedule_node node[CHROME9_PS_MAX_INSTRUCTIONS] = { 0 };
   uint64_t definition[CHROME9_PS_MAX_VIRTUAL_TEMPS]
                      [CHROME9_PS_SCHEDULE_WORDS] = { 0 };
   uint64_t reachable[CHROME9_PS_SCHEDULE_WORDS] = { 0 };
   uint8_t defined_components[CHROME9_PS_MAX_VIRTUAL_TEMPS] = { 0 };
   unsigned total_uses[CHROME9_PS_MAX_VIRTUAL_TEMPS] = { 0 };
   struct chrome9_ps_ir original, candidate;
   unsigned color = UINT_MAX;
   unsigned best_peak;

   if (!input || !output || !input->count ||
       input->count > CHROME9_PS_MAX_INSTRUCTIONS)
      return false;
   original = *input;

   /* Collect every disjoint partial definition before building edges.  A
    * source depends on the complete virtual vector.  That is conservative
    * for lane-independent code and prevents a physical register from being
    * released between partial definitions. */
   for (unsigned i = 0; i < original.count; i++) {
      const struct chrome9_ps_instruction *instruction =
         &original.instruction[i];
      const struct chrome9_ps_destination *destination =
         &instruction->destination;

      if (!source_count(instruction->op) ||
          instruction->op == CHROME9_PS_OPCODE_KILL ||
          !destination->writemask || destination->writemask > 0xf)
         return false;
      if (destination->file == CHROME9_PS_DESTINATION_COLOR) {
         if (color != UINT_MAX || destination->id ||
             destination->writemask != 0xf || i + 1 != original.count)
            return false;
         color = i;
      } else if (destination->file == CHROME9_PS_DESTINATION_TEMP) {
         unsigned temp = destination->id;

         if (temp >= CHROME9_PS_MAX_VIRTUAL_TEMPS ||
             (defined_components[temp] & destination->writemask))
            return false;
         defined_components[temp] |= destination->writemask;
         set_bit(definition[temp], i);
      } else {
         return false;
      }
   }
   if (color == UINT_MAX)
      return false;

   for (unsigned i = 0; i < original.count; i++) {
      const struct chrome9_ps_instruction *instruction =
         &original.instruction[i];

      for (unsigned s = 0; s < source_count(instruction->op); s++) {
         const struct chrome9_ps_source *source = &instruction->source[s];

         if (source->file != CHROME9_PS_SOURCE_TEMP)
            continue;
         if (source->id >= CHROME9_PS_MAX_VIRTUAL_TEMPS ||
             !defined_components[source->id] ||
             (instruction->destination.file == CHROME9_PS_DESTINATION_TEMP &&
              instruction->destination.id == source->id))
            return false;
         for (unsigned w = 0; w < CHROME9_PS_SCHEDULE_WORDS; w++)
            node[i].dependency[w] |= definition[source->id][w];
         if (test_bit(node[i].dependency, i))
            return false;
         total_uses[source->id]++;
      }
   }

   /* All work must contribute to the sole COLOR result.  Besides rejecting
    * dead code, this proves the COLOR record remains terminal under every
    * valid topological order. */
   set_bit(reachable, color);
   for (unsigned pass = 0; pass < original.count; pass++) {
      bool progress = false;

      for (unsigned i = 0; i < original.count; i++) {
         if (!test_bit(reachable, i))
            continue;
         for (unsigned d = 0; d < original.count; d++) {
            if (test_bit(node[i].dependency, d) &&
                !test_bit(reachable, d)) {
               set_bit(reachable, d);
               progress = true;
            }
         }
      }
      if (!progress)
         break;
   }
   for (unsigned i = 0; i < original.count; i++)
      if (!test_bit(reachable, i))
         return false;

   /* Height is a deterministic critical-path tie breaker.  Dependencies may
    * point forward because all partial definitions are grouped, so iterate to
    * a fixed point rather than relying on source order. */
   for (unsigned pass = 0; pass < original.count; pass++) {
      bool progress = false;

      for (unsigned i = 0; i < original.count; i++) {
         unsigned height = 1;

         for (unsigned successor = 0; successor < original.count;
              successor++)
            if (test_bit(node[successor].dependency, i) &&
                height <= node[successor].height)
               height = node[successor].height + 1;
         if (height > node[i].height) {
            node[i].height = height;
            progress = true;
         }
      }
      if (!progress)
         break;
   }

   if (!chrome9_schedule_ps_attempt(&original, node, total_uses, output,
                                    &best_peak, 0))
      return false;
   if (best_peak > CHROME9_PS_SCHEDULE_TARGET) {
      for (unsigned variant = 1;
           variant <= CHROME9_PS_SCHEDULE_VARIANTS; variant++) {
         unsigned candidate_peak;

         if (!chrome9_schedule_ps_attempt(&original, node, total_uses,
                                          &candidate, &candidate_peak,
                                          variant))
            return false;
         if (candidate_peak < best_peak) {
            *output = candidate;
            best_peak = candidate_peak;
         }
         if (best_peak <= CHROME9_PS_SCHEDULE_TARGET)
            break;
      }
   }
   if (peak_live)
      *peak_live = best_peak;
   return true;
}
