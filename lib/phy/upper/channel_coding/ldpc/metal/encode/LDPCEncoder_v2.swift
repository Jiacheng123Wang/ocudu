import Metal
import Foundation

struct LDPCParams {
    var num_bits: UInt32
    var num_chunks: UInt32
}

class LDPCEncoder {
    private let device: MTLDevice
    private let commandQueue: MTLCommandQueue
    private let pipelineState: MTLComputePipelineState
    
    // 极速版 2.0 Shader：引入 threadgroup 缓存
    private let shaderSource = """
    #include <metal_stdlib>
    using namespace metal;

    struct LDPCParams {
        uint32_t num_bits;
        uint32_t num_chunks;
    };

    kernel void ldpc_encode_v2(
        device const uint32_t* g_matrix    [[ buffer(0) ]], 
        device const uint32_t* bit_in      [[ buffer(1) ]], 
        device uint8_t* bit_out            [[ buffer(2) ]], 
        constant LDPCParams&   params      [[ buffer(3) ]],
        uint tid [[ thread_position_in_grid ]],
        uint tgid [[ thread_index_in_threadgroup ]],
        uint tgs  [[ threads_per_threadgroup ]]) 
    {
        // 1. 定义线程组共享内存 (最大支持 512 个 uint32，覆盖 BG1 所有情况)
        threadgroup uint32_t shared_input[264];

        // 2. 协作搬运：组内所有线程参与把 bit_in 搬到 shared_input
        for (uint i = tgid; i < params.num_chunks; i += tgs) {
            shared_input[i] = bit_in[i];
        }

        // 3. 内存屏障：确保搬运完成
        threadgroup_barrier(mem_flags::mem_threadgroup);

        // 4. 边界检查
        if (tid >= params.num_bits) return;

        // 5. 核心计算：从共享内存 (shared_input) 读取，而非 bit_in
        uint row_offset = tid * params.num_chunks;
        uint32_t parity = 0;

        for (uint j = 0; j < params.num_chunks; j++) {
            // 这里不再访问 device 内存，而是访问本地高速缓存
            parity ^= (popcount(g_matrix[row_offset + j] & shared_input[j]) & 1);
        }

        bit_out[tid] = (uint8_t)parity;
    }
    """
    
    init(device: MTLDevice) throws {
        self.device = device
        self.commandQueue = device.makeCommandQueue()!
        let library = try device.makeLibrary(source: shaderSource, options: nil)
        let function = library.makeFunction(name: "ldpc_encode_v2")!
        self.pipelineState = try device.makeComputePipelineState(function: function)
    }
    
    func encode(gBuffer: MTLBuffer, inBuffer: MTLBuffer, outBuffer: MTLBuffer, numBits: Int, numChunks: Int) {
        var params = LDPCParams(num_bits: UInt32(numBits), num_chunks: UInt32(numChunks))
        
        guard let commandBuffer = commandQueue.makeCommandBuffer(),
              let encoder = commandBuffer.makeComputeCommandEncoder() else { return }
        
        encoder.setComputePipelineState(pipelineState)
        encoder.setBuffer(gBuffer, offset: 0, index: 0)
        encoder.setBuffer(inBuffer, offset: 0, index: 1)
        encoder.setBuffer(outBuffer, offset: 0, index: 2)
        encoder.setBytes(&params, length: MemoryLayout<LDPCParams>.size, index: 3)
        
        // --- 极速版 2.0 线程布局 ---
        // 设置组大小为 128 (Apple GPU 推荐)
        let threadsPerGroup = 64
        // 确保 gridSize 是线程组大小的整数倍，以利用满载效率
        let groupsNeeded = (numBits + threadsPerGroup - 1) / threadsPerGroup
        let gridSize = MTLSize(width: groupsNeeded * threadsPerGroup, height: 1, depth: 1)
        let groupSize = MTLSize(width: threadsPerGroup, height: 1, depth: 1)
        
        encoder.dispatchThreadgroups(gridSize, threadsPerThreadgroup: groupSize)
        
        encoder.endEncoding()
        commandBuffer.commit()
        commandBuffer.waitUntilCompleted()
    }
}
