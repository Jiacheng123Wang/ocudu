import Metal
import Foundation

// 参数结构体：必须与 Metal 中的布局严格一致
struct LDPCParams {
    var num_bits: UInt32
    var num_chunks: UInt32
    var g0_words_per_row: UInt32
}

class LDPCEncoder {
    private let device: MTLDevice
    private let commandQueue: MTLCommandQueue
    private let pipelineState: MTLComputePipelineState
    
    // 极速版 3.0：1 线程/1 比特架构 + G0 掩码跳跃读取
    private let shaderSource = """
    #include <metal_stdlib>
    using namespace metal;

    struct LDPCParams {
        uint32_t num_bits;
        uint32_t num_chunks;
        uint32_t g0_words_per_row;
    };

    kernel void ldpc_encode_v3(
        device const uint32_t* g_matrix    [[ buffer(0) ]], 
        device const uint32_t* g0_matrix   [[ buffer(1) ]], 
        device const uint32_t* bit_in      [[ buffer(2) ]], 
        device uint8_t* bit_out            [[ buffer(3) ]], 
        constant LDPCParams&   params      [[ buffer(4) ]],
        uint tid [[ thread_position_in_grid ]],
        uint tgid [[ thread_index_in_threadgroup ]],
        uint tgs  [[ threads_per_threadgroup ]]) 
    {
        // 1. 搬运输入数据到线程组共享内存 (L1 级速度)
        threadgroup uint32_t shared_input[264]; // 兼容 Z=384 的最大 chunks
        for (uint i = tgid; i < params.num_chunks; i += tgs) {
            shared_input[i] = bit_in[i];
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);

        if (tid >= params.num_bits) return;

        // 2. 定位 G 矩阵和 G0 掩码行的起始位置
        uint g_row_offset = tid * params.num_chunks;
        uint g0_row_offset = tid * params.g0_words_per_row;
        
        uint32_t parity = 0;

        // 3. 极速循环：根据 G0 的位图只读取非零的 G word
        for (uint i = 0; i < params.g0_words_per_row; i++) {
            uint32_t mask = g0_matrix[g0_row_offset + i];
            
            // 使用硬件指令 ctz 瞬间定位下一个 1 的位置
            while (mask != 0) {
                uint bit_pos = ctz(mask); 
                uint j = (i << 5) + bit_pos; // 换算成 G 矩阵的 word 索引
                
                if (j < params.num_chunks) {
                    // 仅在此处发生对 G 矩阵 (Device Memory) 的读取
                    parity ^= (popcount(g_matrix[g_row_offset + j] & shared_input[j]) & 1);
                }
                mask &= ~(1u << bit_pos); // 清除已处理位
            }
        }

        bit_out[tid] = (uint8_t)parity;
    }
    """
    
    init(device: MTLDevice) throws {
        self.device = device
        self.commandQueue = device.makeCommandQueue()!
        let library = try device.makeLibrary(source: shaderSource, options: nil)
        let function = library.makeFunction(name: "ldpc_encode_v3")!
        self.pipelineState = try device.makeComputePipelineState(function: function)
    }
    
    func encode(gBuffer: MTLBuffer, g0Buffer: MTLBuffer, inBuffer: MTLBuffer, outBuffer: MTLBuffer, params: LDPCParams) {
        var p = params
        guard let commandBuffer = commandQueue.makeCommandBuffer(),
              let encoder = commandBuffer.makeComputeCommandEncoder() else { return }
        
        encoder.setComputePipelineState(pipelineState)
        encoder.setBuffer(gBuffer, offset: 0, index: 0)
        encoder.setBuffer(g0Buffer, offset: 0, index: 1)
        encoder.setBuffer(inBuffer, offset: 0, index: 2)
        encoder.setBuffer(outBuffer, offset: 0, index: 3)
        encoder.setBytes(&p, length: MemoryLayout<LDPCParams>.size, index: 4)
        
        // 保持 2.0 布局：1 线程对应 1 个 row (输出 bit)
        let threadsPerGroup = 128
        let groupsNeeded = (Int(params.num_bits) + threadsPerGroup - 1) / threadsPerGroup
        let gridSize = MTLSize(width: groupsNeeded * threadsPerGroup, height: 1, depth: 1)
        let groupSize = MTLSize(width: threadsPerGroup, height: 1, depth: 1)
        
        encoder.dispatchThreadgroups(gridSize, threadsPerThreadgroup: groupSize)
        encoder.endEncoding()
        commandBuffer.commit()
        commandBuffer.waitUntilCompleted()
    }
}
