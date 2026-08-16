#import <Metal/Metal.h>
#include "MetalDecoderSUpdate.h"
#include <iostream>

// 隐藏的上下文字典，保存 Metal 的核心状态
struct MetalDecoderSUpdate::MetalContext {
    id<MTLDevice> device;
    id<MTLCommandQueue> commandQueue;
    id<MTLComputePipelineState> pipelineState;
};

MetalDecoderSUpdate::MetalDecoderSUpdate() {
    ctx = new MetalContext();
    
    // 1. 获取默认的 Apple Silicon GPU (M 系列芯片)
    ctx->device = MTLCreateSystemDefaultDevice();
    if (!ctx->device) {
        std::cerr << "Failed to find Metal device!" << std::endl;
        return;
    }
    
    // 2. 创建命令队列
    ctx->commandQueue = [ctx->device newCommandQueue];
    
    // 3. 加载 default.metallib 中的 Kernel 函数
    // 注意：你需要将前面的 Metal 代码编译成 default.metallib 并放在可执行文件同级目录
    NSError* error = nil;
    id<MTLLibrary> defaultLibrary = [ctx->device newDefaultLibrary];
    if (!defaultLibrary) {
        std::cerr << "Failed to load default library." << std::endl;
        return;
    }
    
    id<MTLFunction> updateFunction = [defaultLibrary newFunctionWithName:@"ldpc_decode_s_update_v8"];
    
    // 4. 创建计算管线状态 (Pipeline State)
    ctx->pipelineState = [ctx->device newComputePipelineStateWithFunction:updateFunction error:&error];
    if (!ctx->pipelineState) {
        std::cerr << "Failed to created pipeline state: " << [[error localizedDescription] UTF8String] << std::endl;
    }
}

MetalDecoderSUpdate::~MetalDecoderSUpdate() {
    delete ctx;
}

void* MetalDecoderSUpdate::get_device_handle() {
    return (__bridge void*)ctx->device;
}

// --- 核心修改：内存申请 ---
void* MetalDecoderSUpdate::alloc_buffer(void* device_ptr, size_t size) {
    if (!device_ptr || size == 0) return nullptr;
    
    id<MTLDevice> device = (__bridge id<MTLDevice>)device_ptr;
    // 使用 MTLResourceStorageModeShared 开启 Apple Silicon 的统一内存特性
    id<MTLBuffer> buffer = [device newBufferWithLength:size 
                                              options:MTLResourceStorageModeShared];
    
    // 使用 __bridge_retained 将对象的所有权交给 void*，ARC 不再自动释放它
    return (__bridge_retained void*)buffer;
}

// --- 核心修改：内存释放 ---
void MetalDecoderSUpdate::free_buffer(void* mtl_buffer_ptr) {
    if (!mtl_buffer_ptr) return;
    
    // 使用 CFBridgingRelease 或 __bridge_transfer 将所有权转回 ARC
    // 随着作用域结束，ARC 会自动调用 [buffer release]
    id<MTLBuffer> buffer = (__bridge_transfer id<MTLBuffer>)mtl_buffer_ptr;
    buffer = nil; 
}

void MetalDecoderSUpdate::execute_update(
    void* s_llr_mtl_buffer, 
    void* p_llr_mtl_buffer,
    void* p_hard_mtl_buffer, 
    void* p_pred_mtl_buffer,
    void* G_bits_mtl_buffer, 
    void* GT_bits_mtl_buffer,
    void* fixed_part_mtl_buffer,
    const DecodeParams& params) 
{
    // 1. 将 C++ 传进来的 void* 桥接转换回 id<MTLBuffer> 
    // (__bridge 表示不转移内存管理所有权)
    id<MTLBuffer> buf_s_llr      = (__bridge id<MTLBuffer>)s_llr_mtl_buffer;
    id<MTLBuffer> buf_p_llr      = (__bridge id<MTLBuffer>)p_llr_mtl_buffer;
    id<MTLBuffer> buf_p_hard     = (__bridge id<MTLBuffer>)p_hard_mtl_buffer;
    id<MTLBuffer> buf_p_pred     = (__bridge id<MTLBuffer>)p_pred_mtl_buffer;
    id<MTLBuffer> buf_G_bits     = (__bridge id<MTLBuffer>)G_bits_mtl_buffer;
    id<MTLBuffer> buf_GT_bits    = (__bridge id<MTLBuffer>)GT_bits_mtl_buffer;
    id<MTLBuffer> buf_fixed_part = (__bridge id<MTLBuffer>)fixed_part_mtl_buffer;

    // 2. 创建本次指令的 CommandBuffer 和 Encoder
    id<MTLCommandBuffer> commandBuffer = [ctx->commandQueue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
    
    [encoder setComputePipelineState:ctx->pipelineState];
    
    // 3. 绑定所有的 Buffer (对应 Kernel 中的 [[ buffer(0) ]] 到 [[ buffer(6) ]])
    [encoder setBuffer:buf_s_llr      offset:0 atIndex:0];
    [encoder setBuffer:buf_p_llr      offset:0 atIndex:1];
    [encoder setBuffer:buf_p_hard     offset:0 atIndex:2];
    [encoder setBuffer:buf_p_pred     offset:0 atIndex:3];
    [encoder setBuffer:buf_G_bits     offset:0 atIndex:4];
    [encoder setBuffer:buf_GT_bits    offset:0 atIndex:5];
    [encoder setBuffer:buf_fixed_part offset:0 atIndex:6];
    
    // 4. 将 DecodeParams 结构体直接作为常数按 Bytes 传入 [[ buffer(7) ]]
    [encoder setBytes:&params length:sizeof(DecodeParams) atIndex:7];
    
    // =========================================================
    // 5. 极其关键的线程网格 (Grid) 尺寸设置
    // =========================================================
    // 在 Metal 中，我们设计的逻辑是：一个 Threadgroup (32人) 负责一个 s_idx (gid)
    MTLSize threadsPerThreadgroup = MTLSizeMake(32, 1, 1);
    
    // Grid 的大小就是 s 比特的总数。
    // 注意这里使用的是 dispatchThreadgroups，这样 kernel 里的 gid 才会刚好对应 0 ~ num_s-1
    MTLSize threadgroupsPerGrid = MTLSizeMake(params.num_target, 1, 1);
    
    [encoder dispatchThreadgroups:threadgroupsPerGrid 
            threadsPerThreadgroup:threadsPerThreadgroup];
    
    // 6. 结束编码并提交执行
    [encoder endEncoding];
    [commandBuffer commit];
    
    // 7. 阻塞等待 GPU 执行完毕 (如果是迭代中间步，必须等算完才能算下一步)
    [commandBuffer waitUntilCompleted]; 
}