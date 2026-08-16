#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <unordered_map>
#include "MetalEngineUint4.h"

/**
 * 内部管理类：专门负责 uint4 架构的任务调度与 Buffer 缓存
 */
@interface MetalEngineUint4Internal : NSObject {
@public
    // 缓存 Buffer 指针，避免频繁创建导致的 1ms+ 驱动开销
    std::unordered_map<const void*, id<MTLBuffer>> buffer_cache;
}
@property (strong) id<MTLDevice> device;
@property (strong) id<MTLCommandQueue> command_queue;
@property (strong) id<MTLComputePipelineState> pipeline_gf2_uint4;

- (id<MTLBuffer>)getOrCreateBuffer:(const void*)ptr length:(size_t)length;
@end

@implementation MetalEngineUint4Internal
- (id<MTLBuffer>)getOrCreateBuffer:(const void*)ptr length:(size_t)length {
    auto it = buffer_cache.find(ptr);
    if (it != buffer_cache.end()) return it->second;
    
    // 使用 NoCopy 模式直接映射对齐的内存，消除内存拷贝耗时
    id<MTLBuffer> buffer = [self.device newBufferWithBytesNoCopy:(void*)ptr 
                                                          length:length 
                                                         options:MTLResourceStorageModeShared 
                                                     deallocator:nil];
    if (buffer) buffer_cache[ptr] = buffer;
    return buffer;
}
@end

// --- C 接口实现 ---

void* init_metal_engine_uint4(const char* shader_path) {
    MetalEngineUint4Internal *internal = [[MetalEngineUint4Internal alloc] init];
    internal.device = MTLCreateSystemDefaultDevice();
    internal.command_queue = [internal.device newCommandQueue];

    NSError *error = nil;
    NSString *source = [NSString stringWithContentsOfFile:[NSString stringWithUTF8String:shader_path] 
                                                 encoding:NSUTF8StringEncoding error:&error];
    if (!source) {
        NSLog(@"❌ 无法读取 Shader 文件: %@", error.localizedDescription);
        return nullptr;
    }

    id<MTLLibrary> library = [internal.device newLibraryWithSource:source options:nil error:&error];
    if (!library) {
        // 关键修复：如果 Metal 代码有语法错误，在这里就会被拦截并打印，而不是向后引发崩溃
        NSLog(@"❌ Metal Shader 编译失败:\n%@", error.localizedDescription);
        return nullptr;
    }
    
    id<MTLFunction> func = [library newFunctionWithName:@"gf2_multiply_uint4_extreme"];
    if (!func) {
        NSLog(@"❌ 找不到指定的 kernel 函数：gf2_multiply_uint4_extreme");
        return nullptr;
    }

    internal.pipeline_gf2_uint4 = [internal.device newComputePipelineStateWithFunction:func error:&error];
    if (error) {
        NSLog(@"❌ 创建 Pipeline 失败: %@", error.localizedDescription);
        return nullptr;
    }

    return (__bridge_retained void*)internal;
}

/**
 * @brief 128位向量化 GF(2) 矩阵乘法
 * @param num_rows 必须已补齐至 128 的倍数
 * @param words_per_row 每一行的 uint32 数量，必须是 4 的倍数 (即 128bit 对齐)
 */
void metal_compute_gf2_uint4(void* engine, 
                             const uint32_t* matrix_packed, 
                             const uint32_t* vector_in_packed, 
                             uint32_t* vector_out_packed, 
                             uint32_t num_rows, 
                             uint32_t words_per_row) {
    
    MetalEngineUint4Internal *internal = (__bridge MetalEngineUint4Internal*)engine;
    
    // 1. 内存长度计算 (均为字节)
    size_t matrix_size = (size_t)num_rows * words_per_row * sizeof(uint32_t);
    size_t input_size  = (size_t)words_per_row * sizeof(uint32_t);
    size_t output_size = (size_t)(num_rows / 32) * sizeof(uint32_t); 

    // 2. 获取或创建 Buffer
    id<MTLBuffer> buf_matrix = [internal getOrCreateBuffer:matrix_packed length:matrix_size];
    id<MTLBuffer> buf_input  = [internal getOrCreateBuffer:vector_in_packed length:input_size];
    id<MTLBuffer> buf_output = [internal getOrCreateBuffer:vector_out_packed length:output_size];

    // 3. 构造 128位 架构参数
    struct {
        uint32_t num_rows;
        uint32_t num_cols_chunks_uint4; // 每行含有 uint4 的个数
    } params = { num_rows, words_per_row / 4 };

    // 4. 编码与提交任务
    id<MTLCommandBuffer> cmd_buffer = [internal.command_queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [cmd_buffer computeCommandEncoder];

    [encoder setComputePipelineState:internal.pipeline_gf2_uint4];
    [encoder setBuffer:buf_matrix offset:0 atIndex:0];
    [encoder setBuffer:buf_input  offset:0 atIndex:1];
    [encoder setBuffer:buf_output offset:0 atIndex:2];
    [encoder setBytes:&params length:sizeof(params) atIndex:3];

    // --- 核心调度逻辑 ---
    // 每个线程组 (SIMD-group) 固定 32 线程，产出 128 个比特 (1个 uint4)
    MTLSize threadgroup_size = MTLSizeMake(32, 1, 1);
    // 网格大小变为总行数的 1/128
    MTLSize threadgroup_count = MTLSizeMake(num_rows / 128, 1, 1);

    [encoder dispatchThreadgroups:threadgroup_count threadsPerThreadgroup:threadgroup_size];
    [encoder endEncoding];
    
    [cmd_buffer commit];
    
    // 目前仍阻塞等待以确保逻辑正确，待全 GPU 闭环后移除
    [cmd_buffer waitUntilCompleted];

    // 只有在 commit 之后且执行完成，这两个值才有效
    NSTimeInterval gpuDuration = cmd_buffer.GPUEndTime - cmd_buffer.GPUStartTime;
    printf("GPU 纯硬件执行耗时: %.2f us\n", gpuDuration * 1e6);
}

void destroy_metal_engine_uint4(void* engine) {
    if (!engine) return;
    MetalEngineUint4Internal *internal = (__bridge_transfer MetalEngineUint4Internal*)engine;
    internal->buffer_cache.clear();
}