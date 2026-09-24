/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Chrome9 contributors */
#include "chrome9_ps_schedule.h"

#include <stdio.h>
#include <string.h>

struct compact_source {
   uint8_t file;
   uint8_t id;
   uint16_t swizzle_base6;
};

struct compact_instruction {
   uint8_t op;
   uint8_t destination_file;
   uint8_t destination_id;
   uint8_t writemask;
   uint8_t sampler;
   struct compact_source source[3];
};

/* Exact 100-record default ioquake3 lightall semantic object after
 * alpha-mode-zero and FACE specialization.  The compact base-6 swizzles are
 * lossless encodings of X/Y/Z/W/ONE/ZERO selectors. */
static const struct compact_instruction lightall[] = {
   {0,0,6,0xf,1,{{3,0,561}}}, {2,0,5,0x7,0,{{3,1,53},{0,6,53}}},
   {2,0,4,0x7,0,{{0,5,53},{2,31,5}}}, {2,0,12,0x7,0,{{3,5,779},{3,2,53}}},
   {2,0,11,0x7,0,{{0,12,53},{2,31,263}}}, {0,0,17,0xf,3,{{3,0,561}}},
   {3,0,16,0x7,0,{{0,17,53},{2,31,521}}}, {2,0,15,0x7,0,{{0,16,53},{2,0,263}}},
   {3,0,14,0x7,0,{{3,5,53},{0,15,53}}}, {8,0,19,0x1,0,{{0,14,53},{0,14,53}}},
   {13,0,18,0x1,0,{{0,19,215}}}, {2,0,13,0x7,0,{{0,14,53},{0,18,5}}},
   {8,0,10,0x1,0,{{0,11,53},{0,13,53}}}, {7,0,9,0x1,0,{{0,10,215},{2,31,863}}},
   {6,0,8,0x1,0,{{0,9,215},{2,30,215}}}, {12,0,7,0x1,0,{{0,8,215}}},
   {2,0,3,0x7,0,{{0,4,53},{0,7,5}}}, {0,0,22,0xf,0,{{3,0,57}}},
   {0,0,26,0xf,4,{{3,0,57}}}, {2,0,25,0xf,0,{{0,26,51},{2,2,51}}},
   {2,0,24,0x7,0,{{0,25,53},{2,31,263}}}, {3,0,23,0x7,0,{{2,30,5},{0,24,53}}},
   {2,0,21,0x7,0,{{0,22,53},{0,23,53}}}, {2,0,32,0x1,0,{{0,25,863},{2,31,431}}},
   {3,0,31,0x1,0,{{2,30,215},{0,32,215}}}, {11,0,33,0x1,0,{{0,31,215}}},
   {2,0,34,0x1,0,{{0,33,215},{2,30,431}}}, {10,0,35,0x1,0,{{0,34,215}}},
   {2,0,30,0x1,0,{{0,35,215},{0,35,215}}}, {2,0,29,0x1,0,{{0,30,215},{0,30,215}}},
   {0,0,52,0xf,2,{{3,0,57}}}, {3,0,51,0x3,0,{{0,52,71},{2,31,539}}},
   {2,0,50,0x3,0,{{0,51,71},{2,1,71}}}, {2,0,49,0x7,0,{{3,3,53},{0,50,5}}},
   {2,0,53,0x7,0,{{3,4,53},{0,50,263}}}, {3,0,48,0x7,0,{{0,49,53},{0,53,53}}},
   {2,0,63,0x1,0,{{0,50,215},{0,50,215}}}, {2,0,62,0x1,0,{{0,63,215},{2,31,431}}},
   {3,0,61,0x1,0,{{2,31,863},{0,62,215}}}, {2,0,65,0x1,0,{{0,50,431},{0,50,431}}},
   {2,0,64,0x1,0,{{0,65,215},{2,31,431}}}, {3,0,60,0x1,0,{{0,61,215},{0,64,215}}},
   {7,0,59,0x1,0,{{0,60,215},{2,30,863}}}, {6,0,58,0x1,0,{{0,59,215},{2,30,215}}},
   {13,0,57,0x1,0,{{0,58,215}}}, {12,0,56,0x1,0,{{0,57,215}}},
   {2,0,55,0x7,0,{{0,12,53},{0,56,5}}}, {2,0,54,0x7,0,{{0,55,53},{2,31,263}}},
   {3,0,47,0x7,0,{{0,48,53},{0,54,53}}}, {8,0,67,0x1,0,{{0,47,53},{0,47,53}}},
   {13,0,66,0x1,0,{{0,67,215}}}, {2,0,46,0x7,0,{{0,47,53},{0,66,5}}},
   {1,0,71,0x1,0,{{3,2,863}}}, {1,0,71,0x2,0,{{3,3,1223}}},
   {1,0,71,0x4,0,{{3,4,1283}}}, {8,0,73,0x1,0,{{0,71,53},{0,71,53}}},
   {13,0,72,0x1,0,{{0,73,215}}}, {2,0,70,0x7,0,{{0,71,53},{0,72,5}}},
   {3,0,69,0x7,0,{{0,13,53},{0,70,53}}}, {8,0,75,0x1,0,{{0,69,53},{0,69,53}}},
   {13,0,74,0x1,0,{{0,75,215}}}, {2,0,68,0x7,0,{{0,69,53},{0,74,5}}},
   {8,0,45,0x1,0,{{0,46,53},{0,68,53}}}, {7,0,44,0x1,0,{{0,45,215},{2,30,863}}},
   {6,0,43,0x1,0,{{0,44,215},{2,30,215}}}, {2,0,42,0x1,0,{{0,43,215},{0,43,215}}},
   {3,0,76,0x1,0,{{0,29,215},{2,31,431}}}, {2,0,41,0x1,0,{{0,42,215},{0,76,215}}},
   {3,0,40,0x1,0,{{0,41,215},{2,30,215}}}, {2,0,39,0x1,0,{{2,30,647},{0,40,215}}},
   {2,0,38,0x1,0,{{0,39,215},{0,40,215}}}, {8,0,82,0x1,0,{{0,70,53},{0,68,53}}},
   {7,0,81,0x1,0,{{0,82,215},{2,30,863}}}, {6,0,80,0x1,0,{{0,81,215},{2,30,215}}},
   {2,0,79,0x1,0,{{0,80,215},{0,80,215}}}, {3,0,83,0x1,0,{{0,35,215},{2,29,215}}},
   {2,0,78,0x1,0,{{0,79,215},{0,83,215}}}, {3,0,77,0x1,0,{{0,78,215},{2,29,431}}},
   {2,0,37,0x1,0,{{0,38,215},{0,77,215}}}, {12,0,36,0x1,0,{{0,37,215}}},
   {2,0,28,0x1,0,{{0,29,215},{0,36,215}}}, {2,0,27,0x7,0,{{0,25,53},{0,28,5}}},
   {3,0,20,0x7,0,{{0,21,53},{0,27,53}}}, {2,0,2,0x7,0,{{0,3,53},{0,20,53}}},
   {8,0,86,0x1,0,{{0,46,53},{0,13,53}}}, {7,0,85,0x1,0,{{0,86,215},{2,30,863}}},
   {6,0,84,0x1,0,{{0,85,215},{2,30,215}}}, {2,0,1,0x7,0,{{0,2,53},{0,84,5}}},
   {7,0,93,0x1,0,{{0,10,215},{2,30,863}}}, {6,0,92,0x1,0,{{0,93,215},{2,30,215}}},
   {2,0,91,0x7,0,{{0,3,53},{0,92,5}}}, {2,0,90,0x7,0,{{0,91,53},{2,31,263}}},
   {3,0,89,0x7,0,{{0,5,53},{0,90,53}}}, {7,0,88,0x7,0,{{0,89,53},{2,30,779}}},
   {2,0,87,0x7,0,{{0,88,53},{0,21,53}}}, {3,0,0,0x7,0,{{0,1,53},{0,87,53}}},
   {2,0,94,0x1,0,{{0,22,863},{3,1,863}}}, {1,0,95,0x7,0,{{0,0,53}}},
   {1,0,95,0x8,0,{{0,94,1290}}}, {1,2,0,0xf,0,{{0,95,51}}},
};

static unsigned
source_count(enum chrome9_ps_opcode op)
{
   if (op == CHROME9_PS_OPCODE_TEX || op == CHROME9_PS_OPCODE_MOV ||
       op == CHROME9_PS_OPCODE_EXP2 || op == CHROME9_PS_OPCODE_LOG2 ||
       op == CHROME9_PS_OPCODE_RCP || op == CHROME9_PS_OPCODE_RSQ ||
       op == CHROME9_PS_OPCODE_ABS || op == CHROME9_PS_OPCODE_FRC ||
       op == CHROME9_PS_OPCODE_KILL)
      return 1;
   if (op == CHROME9_PS_OPCODE_MAD || op == CHROME9_PS_OPCODE_CMP)
      return 3;
   return 2;
}

static void
decode_swizzle(uint16_t packed, uint8_t swizzle[4])
{
   for (int lane = 3; lane >= 0; lane--) {
      swizzle[lane] = packed % 6;
      packed /= 6;
   }
}

static void
build_lightall(struct chrome9_ps_ir *ir)
{
   memset(ir, 0, sizeof(*ir));
   ir->count = sizeof(lightall) / sizeof(lightall[0]);
   for (unsigned i = 0; i < ir->count; i++) {
      const struct compact_instruction *compact = &lightall[i];
      struct chrome9_ps_instruction *instruction = &ir->instruction[i];

      instruction->op = compact->op;
      instruction->destination.file = compact->destination_file;
      instruction->destination.id = compact->destination_id;
      instruction->destination.writemask = compact->writemask;
      instruction->sampler = compact->sampler;
      for (unsigned s = 0; s < source_count(instruction->op); s++) {
         instruction->source[s].file = compact->source[s].file;
         instruction->source[s].id = compact->source[s].id;
         decode_swizzle(compact->source[s].swizzle_base6,
                        instruction->source[s].swizzle);
      }
   }
}

static bool
same_multiset(const struct chrome9_ps_ir *a, const struct chrome9_ps_ir *b)
{
   bool matched[CHROME9_PS_MAX_INSTRUCTIONS] = { 0 };

   if (a->count != b->count)
      return false;
   for (unsigned i = 0; i < a->count; i++) {
      bool found = false;

      for (unsigned j = 0; j < b->count; j++) {
         if (!matched[j] &&
             !memcmp(&a->instruction[i], &b->instruction[j],
                     sizeof(a->instruction[i]))) {
            matched[j] = true;
            found = true;
            break;
         }
      }
      if (!found)
         return false;
   }
   return true;
}

int
main(void)
{
   struct chrome9_ps_ir input, scheduled, rejected;
   unsigned peak = 0;
   unsigned tex = 0;

   build_lightall(&input);
   if (input.count != 100 ||
       !chrome9_schedule_ps_ir(&input, &scheduled, &peak) ||
       peak > 12 || scheduled.count != input.count ||
       scheduled.instruction[scheduled.count - 1].destination.file !=
          CHROME9_PS_DESTINATION_COLOR ||
       !same_multiset(&input, &scheduled)) {
      fprintf(stderr, "lightall schedule failed: count=%u peak=%u\n",
              scheduled.count, peak);
      return 1;
   }
   for (unsigned i = 0; i < scheduled.count; i++)
      tex += scheduled.instruction[i].op == CHROME9_PS_OPCODE_TEX;
   if (tex != 5) {
      fprintf(stderr, "lightall texture count changed: %u\n", tex);
      return 1;
   }

   rejected = input;
   rejected.instruction[0].op = CHROME9_PS_OPCODE_KILL;
   if (chrome9_schedule_ps_ir(&rejected, &scheduled, NULL)) {
      fprintf(stderr, "KILL schedule was not rejected\n");
      return 1;
   }
   rejected = input;
   rejected.instruction[1].source[1].file = CHROME9_PS_SOURCE_CONSTANT;
   rejected.instruction[1].source[1].id = 0;
   if (chrome9_schedule_ps_ir(&rejected, &scheduled, NULL)) {
      fprintf(stderr, "dead texture result was not rejected\n");
      return 1;
   }

   printf("chrome9 ps schedule: lightall count=100 alu=95 tex=5 peak=%u\n",
          peak);
   return 0;
}
