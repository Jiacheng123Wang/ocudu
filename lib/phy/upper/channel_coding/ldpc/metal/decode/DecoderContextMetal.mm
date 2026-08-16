#import <Metal/Metal.h>
#include "DecoderContextMetal.h"

/**
 * @brief 实现 sync_cpu_pointers
 * 这里的核心逻辑是调用 [MTLBuffer contents]，它返回一个指向统一内存的 CPU 可访问指针。
 */
void DecoderContext::sync_cpu_pointers() {
    // 内部 Lambda：安全转换并获取地址
    auto get_contents = [](void* handle) -> void* {
        if (!handle) return nullptr;
        // 使用 __bridge 访问 id<MTLBuffer>，不改变引用计数
        id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)handle;
        return [buffer contents];
    };

    s_llr_ptr = (float*)get_contents(buf_s_llr);
    p_llr_ptr = (float*)get_contents(buf_p_llr);
    
    s_hard_ptr = (uint32_t*)get_contents(buf_s_hard);
    p_hard_ptr = (uint32_t*)get_contents(buf_p_hard);
    
    p_pred_ptr = (uint8_t*)get_contents(buf_p_pred);
    s_pred_ptr = (uint8_t*)get_contents(buf_s_pred);
    
    degree_s_ptr = (uint32_t*)get_contents(buf_degree_s);
    degree_p_ptr = (uint32_t*)get_contents(buf_degree_p);
}