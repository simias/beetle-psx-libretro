#ifndef __MDFN_PSX_GTE_H
#define __MDFN_PSX_GTE_H

// Increased accuracy values
typedef struct {
  bool  valid;
  float x;
  float y;
  float z;
} gte_precision;

void GTE_Init(void);
void GTE_Power(void);
int GTE_StateAction(StateMem *sm, int load, int data_only);

const gte_precision *GTE_get_precise(unsigned int which);

int32 GTE_Instruction(uint32_t instr);

void GTE_WriteCR(unsigned int which, uint32_t value);
void GTE_WriteDR(unsigned int which, uint32_t value);

uint32_t GTE_ReadCR(unsigned int which);
uint32_t GTE_ReadDR(unsigned int which);

#endif
