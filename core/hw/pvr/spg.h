#pragma once
#include "ta_ctx.h"

extern bool SH4FastEnough;
extern bool FrameOverBudget;  // True if the last frame exceeded hardware cycle budget

bool spg_Init();
void spg_Term();
void spg_Reset(bool Manual);
void spg_Serialize(Serializer& ser);
void spg_Deserialize(Deserializer& deser);

void CalculateSync();
void read_lightgun_position(int x, int y);
void scheduleRenderDone(TA_context *cntx);
void rescheduleSPG();
