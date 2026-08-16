import Metal
import Foundation

// 参数结构体：必须与 Metal 中的布局严格一致
struct LDPCParams {
    var num_bits: UInt32
    var num_chunks: UInt32
    var num_full_loops: UInt32
    var remainder: UInt32
}

class LDPCEncoder {
    private let device: MTLDevice
    private let commandQueue: MTLCommandQueue
    private let pipelineState: MTLComputePipelineState
    
    // 极速版 4.0：32线程/1比特架构 + 硬件级 SIMD 归约
    private let shaderSource = """
    #include <metal_stdlib>
    using namespace metal;

    struct LDPCParams {
        uint32_t num_bits;
        uint32_t num_chunks;
        uint32_t num_full_loops;
        uint32_t remainder;
    };

    kernel void ldpc_encode_v4_generalized(
        device const uint32_t* g_matrix    [[ buffer(0) ]], 
        constant uint32_t* bit_in          [[ buffer(1) ]], 
        device uint8_t* bit_out            [[ buffer(2) ]], 
        constant LDPCParams&   params      [[ buffer(3) ]],
        uint tid [[ thread_index_in_threadgroup ]],          // 0 - 31 (SIMD lane ID)
        uint gid [[ threadgroup_position_in_grid ]])         // Row ID (0 - 11775)
    {
        // 每个组 (32线程) 协作计算 1 个输出比特
        uint row = gid;
        if (row >= params.num_bits) return;

        // 动态定位 G 矩阵的行起点
        uint g_row_offset = row * params.num_chunks;
        uint32_t partial_parity = 0;

        // --- 第一部分：全员冲刺 (Full Loops) ---
        // 32 个线程并排读取，触发 M4 Pro 的连续显存访问 (Coalesced Access)
        for (uint i = 0; i < params.num_full_loops; i++) {
            uint j = tid + (i << 5); 
            partial_parity ^= popcount(g_matrix[g_row_offset + j] & bit_in[j]);
        }

        // --- 第二部分：精准收尾 (Remainder) ---
        // 只有编号小于余数的线程参与最后不足 32 的那部分计算
        if (tid < params.remainder) {
            uint j = tid + (params.num_full_loops << 5);
            partial_parity ^= popcount(g_matrix[g_row_offset + j] & bit_in[j]);
        }

        // --- 第三部分：硬件级瞬间归约 ---
        // 利用 Apple GPU 硬件原语，32个线程在寄存器间交换数据并做异或汇总
        // 这一行代码替代了所有复杂的 Barrier 和 Threadgroup 内存操作
        uint32_t final_parity = simd_xor(partial_parity);

        // 由 0 号线程作为代表将 1 bit 结果写回内存
        if (tid == 0) {
            bit_out[row] = (uint8_t)(final_parity & 1);
        }
    }
    """
    
    init(device: MTLDevice) throws {
        self.device = device
        self.commandQueue = device.makeCommandQueue()!
        let library = try device.makeLibrary(source: shaderSource, options: nil)
        let function = library.makeFunction(name: "ldpc_encode_v4_generalized")!
        self.pipelineState = try device.makeComputePipelineState(function: function)
    }
    
    func encode(gBuffer: MTLBuffer, inBuffer: MTLBuffer, outBuffer: MTLBuffer, params: LDPCParams) {
        var p = params
        guard let commandBuffer = commandQueue.makeCommandBuffer(),
              let encoder = commandBuffer.makeComputeCommandEncoder() else { return }
        
        encoder.setComputePipelineState(pipelineState)
        encoder.setBuffer(gBuffer, offset: 0, index: 0)
        encoder.setBuffer(inBuffer, offset: 0, index: 1)
        encoder.setBuffer(outBuffer, offset: 0, index: 2)
        encoder.setBytes(&p, length: MemoryLayout<LDPCParams>.size, index: 3)
        
        // --- 4.0 关键布局变化 ---
        // 每个输出比特由一个 32 线程的 Threadgroup 负责
        let threadsPerGroup = 32 
        // 总共有 num_bits 个组
        let gridSize = MTLSize(width: Int(params.num_bits), height: 1, depth: 1)
        let groupSize = MTLSize(width: threadsPerGroup, height: 1, depth: 1)
        
        encoder.dispatchThreadgroups(gridSize, threadsPerThreadgroup: groupSize)
        encoder.endEncoding()
        commandBuffer.commit()
        commandBuffer.waitUntilCompleted()
    }
}
