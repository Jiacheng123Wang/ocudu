#import <Metal/Metal.h>
#import "MetalEngineUpdateLLS.h"
#include <unordered_map>

@interface LLRUpdateInternal : NSObject
@property (strong) id<MTLDevice> device;
@property (strong) id<MTLCommandQueue> commandQueue;
@property (strong) id<MTLComputePipelineState> pipeline;
@property (assign) std::unordered_map<const void*, id<MTLBuffer>> bufferCache;

- (id<MTLBuffer>)getBuffer:(const void*)ptr length:(size_t)length;
@end

@implementation LLRUpdateInternal
- (id<MTLBuffer>)getBuffer:(const void*)ptr length:(size_t)length {
    auto it = _bufferCache.find(ptr);
    if (it != _bufferCache.end()) return it->second;
    
    // Z=256 时，确保内存长度严格对齐到 4096 字节
    size_t alignedLength = (length + 4095) & ~4095;
    
    id<MTLBuffer> buf = [self.device newBufferWithBytesNoCopy:(void*)ptr 
                                                      length:alignedLength 
                                                     options:MTLResourceStorageModeShared 
                                                 deallocator:nil];
    if (!buf) {
        // 如果 NoCopy 失败，退回到创建普通 Buffer，这是诊断是否是对齐问题的关键
        buf = [self.device newBufferWithBytes:ptr length:length options:MTLResourceStorageModeShared];
    }
    _bufferCache[ptr] = buf;
    return buf;
}
@end

void* init_llr_update_engine(const char* shader_path) {
    LLRUpdateInternal* internal = [[LLRUpdateInternal alloc] init];
    internal.device = MTLCreateSystemDefaultDevice();
    if (!internal.device) {
        printf("Error: Metal Device not found.\n");
        return nullptr;
    }
    internal.commandQueue = [internal.device newCommandQueue];
    
    NSError* error = nil;
    NSString* source = [NSString stringWithContentsOfFile:[NSString stringWithUTF8String:shader_path] 
                                                 encoding:NSUTF8StringEncoding error:&error];
    if (error) {
        printf("Error: Cannot read shader file: %s\n", [[error localizedDescription] UTF8String]);
        return nullptr;
    }

    id<MTLLibrary> library = [internal.device newLibraryWithSource:source options:nil error:&error];
    if (error) {
        printf("Error: Metal Library Compilation Failed: %s\n", [[error localizedDescription] UTF8String]);
        return nullptr;
    }

    id<MTLFunction> function = [library newFunctionWithName:@"llr_update_vnode_centric"];
    if (!function) {
        printf("Error: Cannot find kernel 'llr_update_vnode_centric' in shader file.\n");
        return nullptr;
    }
    internal.pipeline = [internal.device newComputePipelineStateWithFunction:function error:&error];
    if (error) {
        printf("Error: Pipeline State creation failed: %s\n", [[error localizedDescription] UTF8String]);
        return nullptr;
    }
    
    printf("Metal Engine Initialized Successfully.\n");

    return (__bridge_retained void*)internal;
}

void metal_update_llr(void* engine,
                      float16* llr_array,
                      const uint32_t* h_matrix,
                      const uint32_t* ht_matrix,
                      const uint32_t* h_pred,
                      const uint16_t* v_degrees,
                      uint32_t num_v_nodes, 
                      uint32_t max_v_degree,
                      const LLRUpdateParams* params) 
{
    if (!engine) return;
    LLRUpdateInternal* internal = (__bridge LLRUpdateInternal*)engine;

    // --- 1. 计算原始尺寸 ---
    uint32_t N = params->num_v_nodes;
    uint32_t M = params->num_h_nodes;
    size_t n_size = N * sizeof(float16);
    size_t h_size = (size_t)M * params->n_h_chunks * sizeof(uint32_t);
    size_t m_chunk_size = (size_t)params->n_ht_chunks * sizeof(uint32_t);
    size_t ht_size = (size_t)N * m_chunk_size;
    size_t deg_size = N * sizeof(uint16_t);

    id<MTLCommandBuffer> cmdBuf = [internal.commandQueue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [cmdBuf computeCommandEncoder];
    [encoder setComputePipelineState:internal.pipeline];

    // --- 2. 关键：使用 getBuffer 触发 newBufferWithBytesNoCopy ---
    // 这将确保 GPU 指针直接指向 test_llr_update.cpp 中分配的内存地址
    [encoder setBuffer:[internal getBuffer:llr_array length:n_size] offset:0 atIndex:0];
    [encoder setBuffer:[internal getBuffer:h_matrix length:h_size] offset:0 atIndex:1];
    [encoder setBuffer:[internal getBuffer:ht_matrix length:ht_size] offset:0 atIndex:2];
    [encoder setBuffer:[internal getBuffer:h_pred length:m_chunk_size] offset:0 atIndex:3];
    [encoder setBuffer:[internal getBuffer:v_degrees length:deg_size] offset:0 atIndex:4];
    
    [encoder setBytes:params length:sizeof(LLRUpdateParams) atIndex:5];
    [encoder setThreadgroupMemoryLength:max_v_degree * sizeof(uint32_t) atIndex:0];

    [encoder dispatchThreadgroups:MTLSizeMake(N, 1, 1) 
            threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];

    [encoder endEncoding];
    [cmdBuf commit];
    [cmdBuf waitUntilCompleted];

    // --- 3. 移除 memcpy ---
    // 在 Shared 模式下，GPU 完成后 CPU 侧 llr_array 内容已同步更新
    
    NSTimeInterval gpuDuration = cmdBuf.GPUEndTime - cmdBuf.GPUStartTime;
    printf("GPU 零拷贝纯硬件执行耗时: %.2f us\n", gpuDuration * 1e6);
}


void deinit_llr_update_engine(void* engine) {
    LLRUpdateInternal* internal = (__bridge_transfer LLRUpdateInternal*)engine;
    internal = nil;
}