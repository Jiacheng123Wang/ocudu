#ifndef COLLABORATION_DECODER_H
#define COLLABORATION_DECODER_H

#include "DecoderContext.h"
#include <cstdint>

// S 空间 LLR 更新
void update_s(DecoderContext& ctx, uint32_t top_n, float power_val);
void update_s_neon(DecoderContext& ctx, uint32_t top_n, float power_val);

// P 空间 LLR 更新
void update_p(DecoderContext& ctx, uint32_t top_n, float power_val);
void update_p_neon(DecoderContext& ctx, uint32_t top_n, float power_val);

// 最大迭代后的最后一搏裁决
void final_decision(DecoderContext& ctx);
void final_decision_neon(DecoderContext& ctx);

#endif // COLLABORATION_DECODER_H