#import "MetalLLSUpdater.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <unordered_map>

struct VNStats {
    uint32_t ErrEqCnt;    // 报错的校验方程数
    uint32_t SuspectCnt;  // 怀疑度计数
    float EvidSum;        // 证据累加和
    float VNTotalCN;      // VN连接的CN总数 (或当前权值)
};


// 内部实现类，负责管理 Metal 状态和 Buffer 缓存
@interface MetalLLSInternal : NSObject
@property (strong) id<MTLDevice> device;
@property (strong) id<MTLCommandQueue> queue;
@property (strong) id<MTLComputePipelineState> pipelineScan;
@property (strong) id<MTLComputePipelineState> pipelineUpdate;
@property (assign) std::unordered_map<const void*, id<MTLBuffer>> bufferCache;

@property (strong) id<MTLBuffer> bufErrEqCnt;
@property (strong) id<MTLBuffer> bufSuspectCnt;
@property (strong) id<MTLBuffer> bufEvidSum;
@property (strong) id<MTLBuffer> bufVNTotalCN;

- (id<MTLBuffer>)getBuffer:(const void*)ptr length:(size_t)length;
@end

@implementation MetalLLSInternal

- (id<MTLBuffer>)getBuffer:(const void*)ptr length:(size_t)length {
    auto it = _bufferCache.find(ptr);
    if (it != _bufferCache.end()) return it->second;
    
    size_t alignedLength = (length + 4095) & ~4095;
    id<MTLBuffer> buf = [_device newBufferWithBytesNoCopy:(void*)ptr 
                                                  length:alignedLength 
                                                 options:MTLResourceStorageModeShared 
                                             deallocator:nil];
    if (!buf) {
        buf = [_device newBufferWithBytes:ptr length:length options:MTLResourceStorageModeShared];
    }
    _bufferCache[ptr] = buf;
    return buf;
}
@end

extern "C" {

void* init_llr_updater_engine(const char* shader_path) {
    MetalLLSInternal* internal = [[MetalLLSInternal alloc] init];
    
    // 1. 检查设备
    internal.device = MTLCreateSystemDefaultDevice();
    if (!internal.device) {
        fprintf(stderr, "Metal Error: Could not find a Metal-capable GPU.\n");
        return nullptr;
    }
    
    internal.queue = [internal.device newCommandQueue];
    
    // 2. 检查 Shader 文件是否存在及读取
    NSError* error = nil;
    NSString* path = [NSString stringWithUTF8String:shader_path];
    NSString* source = [NSString stringWithContentsOfFile:path 
                                                 encoding:NSUTF8StringEncoding 
                                                    error:&error];
    if (error || !source) {
        fprintf(stderr, "Metal Error: Cannot read shader file at path: %s\n", shader_path);
        if (error) fprintf(stderr, "Reason: %s\n", [[error localizedDescription] UTF8String]);
        return nullptr;
    }

    // 3. 检查 Shader 库编译
    id<MTLLibrary> library = [internal.device newLibraryWithSource:source options:nil error:&error];
    if (error || !library) {
        fprintf(stderr, "Metal Error: Shader compilation failed!\n");
        fprintf(stderr, "%s\n", [[error localizedDescription] UTF8String]);
        return nullptr;
    }

    // 4. 检查 Kernel 函数名是否匹配
    id<MTLFunction> funcScan = [library newFunctionWithName:@"cn_centric_scan"];
    id<MTLFunction> funcUpdate = [library newFunctionWithName:@"vn_centric_update"];
    
    if (!funcScan) {
        fprintf(stderr, "Metal Error: Cannot find kernel function 'cn_centric_scan'\n");
        return nullptr;
    }
    if (!funcUpdate) {
        fprintf(stderr, "Metal Error: Cannot find kernel function 'vn_centric_update'\n");
        return nullptr;
    }

    // 5. 检查 Pipeline State 创建
    internal.pipelineScan = [internal.device newComputePipelineStateWithFunction:funcScan error:&error];
    if (error) {
        fprintf(stderr, "Metal Error (Scan Pipeline): %s\n", [[error localizedDescription] UTF8String]);
        return nullptr;
    }
    
    internal.pipelineUpdate = [internal.device newComputePipelineStateWithFunction:funcUpdate error:&error];
    if (error) {
        fprintf(stderr, "Metal Error (Update Pipeline): %s\n", [[error localizedDescription] UTF8String]);
        return nullptr;
    }

    return (__bridge_retained void*)internal;
}

void engine_prepare_internal_buffers(void* engine, const uint32_t* h_raw_ptr, uint32_t M, uint32_t n_h_chunks) {
    MetalLLSInternal* internal = (__bridge MetalLLSInternal*)engine;
    uint32_t n_vn = n_h_chunks * 32;
    size_t uintSize = (n_vn * sizeof(uint32_t) + 4095) & ~4095;
    
    internal.bufErrEqCnt = [internal.device newBufferWithLength:uintSize options:MTLResourceStorageModeShared];
    internal.bufSuspectCnt = [internal.device newBufferWithLength:uintSize options:MTLResourceStorageModeShared];
    internal.bufEvidSum = [internal.device newBufferWithLength:uintSize options:MTLResourceStorageModeShared];
    internal.bufVNTotalCN = [internal.device newBufferWithLength:uintSize options:MTLResourceStorageModeShared];

    uint32_t* total_ptr = (uint32_t*)internal.bufVNTotalCN.contents;
    memset(total_ptr, 0, uintSize);
    for (uint32_t r = 0; r < M; ++r) {
        for (uint32_t c = 0; c < n_h_chunks; ++c) {
            uint32_t mask = h_raw_ptr[r * n_h_chunks + c];
            while (mask) {
                total_ptr[c * 32 + __builtin_ctz(mask)]++;
                mask &= (mask - 1);
            }
        }
    }
}

void engine_update_lls(void* engine, float alpha, void* llr_ptr, const void* h_matrix, const void* h_pred, void* s_hard, uint32_t M, uint32_t n_h_chunks, void* debug_ptr) {
    MetalLLSInternal* internal = (__bridge MetalLLSInternal*)engine;
    uint32_t n_vn = n_h_chunks * 32;

    id<MTLCommandBuffer> cmdBuf = [internal.queue commandBuffer];

    // 1. 修复点：使用 Blit Encoder 专门处理内存清零
    id<MTLBlitCommandEncoder> blitEncoder = [cmdBuf blitCommandEncoder];
    [blitEncoder fillBuffer:internal.bufErrEqCnt range:NSMakeRange(0, internal.bufErrEqCnt.length) value:0];
    [blitEncoder fillBuffer:internal.bufSuspectCnt range:NSMakeRange(0, internal.bufSuspectCnt.length) value:0];
    [blitEncoder fillBuffer:internal.bufEvidSum range:NSMakeRange(0, internal.bufEvidSum.length) value:0];
    [blitEncoder endEncoding]; // 结束 Blit 阶段

    // 2. 开始 Compute 阶段
    id<MTLComputeCommandEncoder> encoder = [cmdBuf computeCommandEncoder];

    // Kernel 1: CN-Centric Scan
    [encoder setComputePipelineState:internal.pipelineScan];
    [encoder setBuffer:[internal getBuffer:h_matrix length:M * n_h_chunks * sizeof(uint32_t)] offset:0 atIndex:0];
    [encoder setBuffer:[internal getBuffer:llr_ptr length:n_vn * 2] offset:0 atIndex:1];
    [encoder setBuffer:[internal getBuffer:h_pred length:M * sizeof(uint32_t)] offset:0 atIndex:2];
    [encoder setBuffer:internal.bufErrEqCnt offset:0 atIndex:3];
    [encoder setBuffer:internal.bufSuspectCnt offset:0 atIndex:4];
    [encoder setBuffer:internal.bufEvidSum offset:0 atIndex:5];
    [encoder setBytes:&n_h_chunks length:sizeof(uint32_t) atIndex:6];

// n_vn 是节点的总数，VNStats 是我们在 .metal 里定义的结构体
    if (debug_ptr) {
        id<MTLBuffer> debugBuf = [internal getBuffer:debug_ptr length:n_vn * sizeof(VNStats)];
        [encoder setBuffer:debugBuf offset:0 atIndex:7];
    }

    [encoder dispatchThreadgroups:MTLSizeMake(M, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
    
    [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];

    // Kernel 2: VN-Centric Update
    [encoder setComputePipelineState:internal.pipelineUpdate];
    [encoder setBuffer:[internal getBuffer:llr_ptr length:n_vn * 2] offset:0 atIndex:0];
    [encoder setBuffer:internal.bufErrEqCnt offset:0 atIndex:1];
    [encoder setBuffer:internal.bufSuspectCnt offset:0 atIndex:2];
    [encoder setBuffer:internal.bufEvidSum offset:0 atIndex:3];
    [encoder setBuffer:internal.bufVNTotalCN offset:0 atIndex:4];
    [encoder setBuffer:[internal getBuffer:s_hard length:(n_vn / 32) * sizeof(uint32_t)] offset:0 atIndex:5];
    [encoder setBytes:&alpha length:sizeof(float) atIndex:6];

    [encoder dispatchThreads:MTLSizeMake(n_vn, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];

    [encoder endEncoding];
    [cmdBuf commit];
    [cmdBuf waitUntilCompleted];

    //NSTimeInterval gpuDuration = cmdBuf.GPUEndTime - cmdBuf.GPUStartTime;
    //printf("GPU 零拷贝纯硬件执行耗时: %.2f us\n", gpuDuration * 1e6);
}

void deinit_llr_updater_engine(void* engine) {
    MetalLLSInternal* internal = (__bridge_transfer MetalLLSInternal*)engine;
    internal.bufferCache.clear();
    internal = nil;
}
}