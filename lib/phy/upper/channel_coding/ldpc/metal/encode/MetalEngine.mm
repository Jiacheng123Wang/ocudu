#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import "MetalEngine.h"
#include <unordered_map>

// 内部实现类，增加 C++ 的哈希表用于缓存 MTLBuffer
@interface MetalInternal : NSObject {
@public
    // 缓存映射：裸指针 -> MTLBuffer 对象
    // 对于 G/F 矩阵：映射一次，终生复用
    // 对于 s_hard/p_hard：虽然 CPU 修改数值，但指针不变，因此也可以复用 Buffer 对象
    std::unordered_map<const void*, id<MTLBuffer>> bufferCache;
}
@property (strong) id<MTLDevice> device;
@property (strong) id<MTLCommandQueue> commandQueue;
@property (strong) id<MTLComputePipelineState> pipelinePredict;   // 输出字节版
@property (strong) id<MTLComputePipelineState> pipelineBitPacked; // 输出位包装版

// 获取或创建缓存的 Buffer，核心是利用 BytesNoCopy
- (id<MTLBuffer>)getOrCreateBuffer:(const void*)ptr length:(size_t)length;
@end

@implementation MetalInternal
- (id<MTLBuffer>)getOrCreateBuffer:(const void*)ptr length:(size_t)length {
    auto it = bufferCache.find(ptr);
    if (it != bufferCache.end()) {
        return it->second;
    }
    
    // 只有第一次见到该指针时，才进行昂贵的页表锁定和 VM 映射
    id<MTLBuffer> buffer = [self.device newBufferWithBytesNoCopy:(void*)ptr 
                                                          length:length 
                                                         options:MTLResourceStorageModeShared 
                                                     deallocator:nil];
    if (buffer) {
        bufferCache[ptr] = buffer;
    }
    return buffer;
}
@end

void* init_metal_engine(const char* shader_path) {
    MetalInternal *internal = [[MetalInternal alloc] init];
    internal.device = MTLCreateSystemDefaultDevice();
    if (!internal.device) return nil;
    internal.commandQueue = [internal.device newCommandQueue];

    NSError *error = nil;
    NSString *source = [NSString stringWithContentsOfFile:[NSString stringWithUTF8String:shader_path] 
                                                 encoding:NSUTF8StringEncoding error:&error];
    if (!source) return nil;

    id<MTLLibrary> library = [internal.device newLibraryWithSource:source options:nil error:&error];
    if (!library) return nil;

    // 分别加载两个不同的 Kernel
    id<MTLFunction> funcPredict = [library newFunctionWithName:@"ldpc_predict_v4"];
    id<MTLFunction> funcBitPacked = [library newFunctionWithName:@"gf2_matrix_vector_multiply"];
    
    internal.pipelinePredict = [internal.device newComputePipelineStateWithFunction:funcPredict error:&error];
    internal.pipelineBitPacked = [internal.device newComputePipelineStateWithFunction:funcBitPacked error:&error];

    return (__bridge_retained void*)internal;
}

void metal_compute_gf2(void* engine, 
                       MetalKernelType type,
                       const uint32_t* matrix, 
                       const uint32_t* vector_in, 
                       void* vector_out, 
                       uint32_t num_bits, 
                       uint32_t num_chunks) {
    
    MetalInternal *internal = (__bridge MetalInternal*)engine;
    
    // 1. 自动计算长度并获取/创建缓存 Buffer
    size_t m_len = (size_t)num_bits * num_chunks * 4;
    size_t i_len = (size_t)num_chunks * 4;
    // 预测模式输出字节(uint8)，位包装模式输出字(uint32)
    size_t o_len = (type == KERNEL_TYPE_PREDICT_V4) ? num_bits : (((num_bits + 31) / 32) * 4);

    id<MTLBuffer> bufM = [internal getOrCreateBuffer:matrix length:m_len];
    id<MTLBuffer> bufI = [internal getOrCreateBuffer:vector_in length:i_len];
    id<MTLBuffer> bufO = [internal getOrCreateBuffer:vector_out length:o_len];

    // 2. 编码命令
    id<MTLCommandBuffer> commandBuffer = [internal.commandQueue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];

    [encoder setBuffer:bufM offset:0 atIndex:0];
    [encoder setBuffer:bufI offset:0 atIndex:1];
    [encoder setBuffer:bufO offset:0 atIndex:2];

    if (type == KERNEL_TYPE_PREDICT_V4) {
        [encoder setComputePipelineState:internal.pipelinePredict];
        struct { uint32_t n, c, f, r; } params = { num_bits, num_chunks, num_chunks/32, num_chunks%32 };
        [encoder setBytes:&params length:sizeof(params) atIndex:3];
        [encoder dispatchThreadgroups:MTLSizeMake(num_bits, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
    } else {
        [encoder setComputePipelineState:internal.pipelineBitPacked];
        struct { uint32_t n, c; } params = { num_bits, num_chunks };
        [encoder setBytes:&params length:sizeof(params) atIndex:3];
        uint32_t groups = (num_bits + 31) / 32;
        [encoder dispatchThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
    }

    [encoder endEncoding];
    [commandBuffer commit];
    
    // 关键：等待同步。虽然 CPU 只读 pred，但它是在 commit 后立即读取的。
    [commandBuffer waitUntilCompleted]; 

    // 只有在 commit 之后且执行完成，这两个值才有效
    // NSTimeInterval gpuDuration = commandBuffer.GPUEndTime - commandBuffer.GPUStartTime;
    // printf("GPU 纯硬件执行耗时: %.2f us\n", gpuDuration * 1e6);
}

void deinit_metal_engine(void* engine) {
    if (engine) {
        MetalInternal *internal = (__bridge_transfer MetalInternal*)engine;
        internal->bufferCache.clear();
        internal = nil;
    }
}