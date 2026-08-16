#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <unordered_map>
#include "ldpc_gpu_decoder.h"

// 严格匹配 Metal 中的 DecodeCtrl 结构
struct DecodeCtrl {
    uint32_t error_count;     // atomic_uint
    uint32_t early_terminate; // atomic_uint
    uint32_t actual_iters;    // atomic_uint
};

struct GPUEngineImpl {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    
    id<MTLComputePipelineState> pInit;
    id<MTLComputePipelineState> pSyndrome;
    id<MTLComputePipelineState> pScan;
    id<MTLComputePipelineState> pUpdateLLRHpred; // 新的 Pipeline    
 
    // 零拷贝缓存池：避免每次 decode 都重新封装 Buffer
    std::unordered_map<const void*, id<MTLBuffer>> bufferCache;
    
    id<MTLBuffer> bufHMatrix;     // 零拷贝
    id<MTLBuffer> bufHTMatrix;    // 新增：HT 矩阵 Buffer
    id<MTLBuffer> bufVNTotalCN;   // 零拷贝 (由 CPU 计算并写入，GPU 只读)
    
    id<MTLBuffer> bufCtrl;        // Shared (GPU写，CPU读)
    
    // --- 极速私有内存 (Private)：完全在 GPU 内部消化，CPU 不可读写 ---
    id<MTLBuffer> bufSHard;       // 【修改】：已改为 Private，仅供 pInit 和 pSyndrome 使用
    id<MTLBuffer> bufHPred;
    id<MTLBuffer> bufErrEqCnt;
    id<MTLBuffer> bufSuspectCnt;
    id<MTLBuffer> bufEvidenceSum;
 
    id<MTLBuffer> bufDebugStats;

    uint32_t N_aligned;
    uint32_t M_aligned;
    uint32_t n_h_chunks;
    uint32_t h_pred_len;
    uint32_t n_info;
    float alpha;
};

// --- 零拷贝封装核心函数 ---
static id<MTLBuffer> get_zero_copy_buffer(GPUEngineImpl* engine, const void* ptr, size_t length) {
    auto it = engine->bufferCache.find(ptr);
    if (it != engine->bufferCache.end()) return it->second;

    size_t alignedLength = (length + 4095) & ~4095;
    id<MTLBuffer> buf = [engine->device newBufferWithBytesNoCopy:(void*)ptr
                                                          length:alignedLength
                                                         options:MTLResourceStorageModeShared
                                                     deallocator:nil];
    if (!buf) {
        buf = [engine->device newBufferWithBytes:ptr length:length options:MTLResourceStorageModeShared];
    }
    engine->bufferCache[ptr] = buf;
    return buf;
}

bool LDPCGPUDecoder::init(const char* metallib_path, uint32_t N_logical, uint32_t M_logical, float alpha) {
    GPUEngineImpl* engine = new GPUEngineImpl();
    this->internal_state = engine;
    
    engine->device = MTLCreateSystemDefaultDevice();
    engine->queue = [engine->device newCommandQueue];
    engine->alpha = alpha;
    
    engine->n_info = N_logical - M_logical;
    
    engine->N_aligned = ((N_logical + 31) / 32) * 32;
    engine->M_aligned = ((M_logical + 31) / 32) * 32;
    engine->n_h_chunks = engine->N_aligned / 32;
    engine->h_pred_len = engine->M_aligned / 32;

    NSURL *libraryURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:metallib_path]];
    NSError* error = nil;
    id<MTLLibrary> library = [engine->device newLibraryWithURL:libraryURL error:&error];
    if (!library) {
        NSLog(@"Metal Library 载入失败: %@", error);
        return false;
    }
    
    engine->pInit = [engine->device newComputePipelineStateWithFunction:[library newFunctionWithName:@"init_hard_decisions"] error:&error];
    engine->pSyndrome = [engine->device newComputePipelineStateWithFunction:[library newFunctionWithName:@"compute_syndrome"] error:&error];
    engine->pScan = [engine->device newComputePipelineStateWithFunction:[library newFunctionWithName:@"cn_centric_scan"] error:&error];
    engine->pUpdateLLRHpred = [engine->device newComputePipelineStateWithFunction:[library newFunctionWithName:@"update_llr_hpred"] error:&error];
    
    engine->bufCtrl = [engine->device newBufferWithLength:sizeof(DecodeCtrl) options:MTLResourceStorageModeShared];
    engine->bufDebugStats = [engine->device newBufferWithLength:engine->N_aligned * sizeof(VNStats) options:MTLResourceStorageModeShared];

    // Private 内存：中间变量，极速模式
    // 【修改】：将 bufSHard 改为 Private 内存
    engine->bufSHard = [engine->device newBufferWithLength:engine->n_h_chunks * sizeof(uint32_t) options:MTLResourceStorageModePrivate];
    engine->bufHPred = [engine->device newBufferWithLength:engine->h_pred_len * sizeof(uint32_t) options:MTLResourceStorageModePrivate];
    engine->bufErrEqCnt = [engine->device newBufferWithLength:engine->N_aligned * sizeof(uint32_t) options:MTLResourceStorageModePrivate];
    engine->bufSuspectCnt = [engine->device newBufferWithLength:engine->N_aligned * sizeof(uint32_t) options:MTLResourceStorageModePrivate];
    engine->bufEvidenceSum = [engine->device newBufferWithLength:engine->N_aligned * sizeof(float) options:MTLResourceStorageModePrivate];

    return true;
}

void LDPCGPUDecoder::load_h_and_ht_matrix(const uint32_t* h_matrix_raw, const uint32_t* ht_matrix_raw, uint32_t* column_weights_out) {
    GPUEngineImpl* engine = (GPUEngineImpl*)internal_state;
    
    size_t h_size = engine->M_aligned * engine->n_h_chunks * sizeof(uint32_t);
    engine->bufHMatrix = get_zero_copy_buffer(engine, h_matrix_raw, h_size);
    
    engine->bufHTMatrix = get_zero_copy_buffer(engine, ht_matrix_raw, engine->N_aligned * engine->h_pred_len * sizeof(uint32_t));  
    
    size_t col_size = engine->N_aligned * sizeof(uint32_t);
    engine->bufVNTotalCN = get_zero_copy_buffer(engine, column_weights_out, col_size);
    
    memset(column_weights_out, 0, col_size);
    for (uint32_t r = 0; r < engine->M_aligned; ++r) {
        const uint32_t* row_base = h_matrix_raw + r * engine->n_h_chunks;
        for (uint32_t c = 0; c < engine->n_h_chunks; ++c) {
            uint32_t mask = row_base[c];
            uint32_t col_base = c * 32;
            while (mask != 0) {
                uint32_t bit = __builtin_ctz(mask);
                if (col_base + bit < engine->N_aligned) {
                    column_weights_out[col_base + bit]++;
                }
                mask &= (mask - 1);
            }
        }
    }
}

int LDPCGPUDecoder::decode(const void* in_sp_llr_fp16, uint8_t* out_s_bits, int max_iter) {
    GPUEngineImpl* engine = (GPUEngineImpl*)internal_state;

    id<MTLBuffer> mtlLLR = get_zero_copy_buffer(engine, in_sp_llr_fp16, engine->N_aligned * sizeof(uint16_t));
    
    id<MTLCommandBuffer> cmdBuf = [engine->queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [cmdBuf computeCommandEncoder];
    
    // --- 阶段 A：预处理 (Init Hard Decisions & 重置全局控制块) ---
    [encoder setComputePipelineState:engine->pInit];
    [encoder setBuffer:mtlLLR offset:0 atIndex:0]; 
    [encoder setBuffer:engine->bufSHard offset:0 atIndex:1];
    [encoder setBuffer:engine->bufCtrl offset:0 atIndex:2];
    [encoder setBuffer:engine->bufErrEqCnt offset:0 atIndex:3];
    [encoder setBuffer:engine->bufSuspectCnt offset:0 atIndex:4];
    [encoder setBuffer:engine->bufEvidenceSum offset:0 atIndex:5];
    [encoder dispatchThreads:MTLSizeMake(engine->N_aligned, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
    
    // 2. 计算初始全量 Syndrome (仅此一次)
    [encoder setComputePipelineState:engine->pSyndrome];
    [encoder setBuffer:engine->bufHMatrix offset:0 atIndex:0];
    [encoder setBuffer:engine->bufSHard offset:0 atIndex:1];
    [encoder setBuffer:engine->bufHPred offset:0 atIndex:2];
    [encoder setBuffer:engine->bufCtrl offset:0 atIndex:3];
    [encoder setBytes:&engine->n_h_chunks length:sizeof(uint32_t) atIndex:4];
    [encoder dispatchThreadgroups:MTLSizeMake(engine->h_pred_len, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];

    // --- 阶段 B：展开循环录制 (GPU 内部消化) ---
    for (int it = 0; it < max_iter; ++it) {
        // 1. CN Scan：统计当前错误并寻找嫌疑人
        [encoder setComputePipelineState:engine->pScan];
        [encoder setBuffer:engine->bufHMatrix offset:0 atIndex:0];
        [encoder setBuffer:mtlLLR offset:0 atIndex:1];
        [encoder setBuffer:engine->bufHPred offset:0 atIndex:2];
        [encoder setBuffer:engine->bufErrEqCnt offset:0 atIndex:3];
        [encoder setBuffer:engine->bufSuspectCnt offset:0 atIndex:4];
        [encoder setBuffer:engine->bufEvidenceSum offset:0 atIndex:5];
        [encoder setBytes:&engine->n_h_chunks length:sizeof(uint32_t) atIndex:6];
        [encoder setBuffer:engine->bufCtrl offset:0 atIndex:7];
        [encoder dispatchThreadgroups:MTLSizeMake(engine->M_aligned, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];

        // 2. Update：执行 LLR 更新，翻转 h_pred，并在最后重置 ctrl 供下一轮使用
        [encoder setComputePipelineState:engine->pUpdateLLRHpred];
        [encoder setBuffer:mtlLLR offset:0 atIndex:0];
        [encoder setBuffer:engine->bufErrEqCnt offset:0 atIndex:1];
        [encoder setBuffer:engine->bufSuspectCnt offset:0 atIndex:2];
        [encoder setBuffer:engine->bufEvidenceSum offset:0 atIndex:3];
        [encoder setBuffer:engine->bufVNTotalCN offset:0 atIndex:4];
        [encoder setBuffer:engine->bufHPred offset:0 atIndex:5];
        [encoder setBuffer:engine->bufHTMatrix offset:0 atIndex:6];
        [encoder setBytes:&engine->h_pred_len length:sizeof(uint32_t) atIndex:7];
        [encoder setBytes:&engine->alpha length:sizeof(float) atIndex:8];
        [encoder setBuffer:engine->bufCtrl offset:0 atIndex:9];
        [encoder setBuffer:engine->bufDebugStats offset:0 atIndex:10];
        
        [encoder dispatchThreads:MTLSizeMake(engine->N_aligned, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
    }
    
    [encoder endEncoding];
    [cmdBuf commit];
    [cmdBuf waitUntilCompleted];
    
    // 2. 核心逻辑：结果提取与解包
    DecodeCtrl* ctrl = (DecodeCtrl*)engine->bufCtrl.contents;
    uint16_t* final_llr = (uint16_t*)mtlLLR.contents;
    
    if (out_s_bits) {
        for (uint32_t i = 0; i < engine->n_info; ++i) {
            out_s_bits[i] = (final_llr[i] >> 15) & 1;
        }
    }
    
    return ctrl->actual_iters;
}

void LDPCGPUDecoder::release() {
    GPUEngineImpl* engine = (GPUEngineImpl*)internal_state;
    if (engine) {
        engine->bufferCache.clear(); 
        delete engine;
        internal_state = nullptr;
    }
}    

VNStats* LDPCGPUDecoder::get_debug_ptr() {
    if (!internal_state) return nullptr;
    GPUEngineImpl* engine = (GPUEngineImpl*)this->internal_state;
    if (engine->bufDebugStats) {
        return (VNStats*)engine->bufDebugStats.contents;
    }
    return nullptr;
}