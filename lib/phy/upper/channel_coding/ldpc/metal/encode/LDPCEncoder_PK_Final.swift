import Metal
import Foundation
import QuartzCore

// 参数结构体：统一增加 v4 所需的预计算参数
struct LDPCParams {
    var num_bits: UInt32
    var num_chunks: UInt32
    var g0_words_per_row: UInt32
    var num_full_loops: UInt32 // v4 专用
    var remainder: UInt32      // v4 专用
}

class LDPCEncoder {
    private let device: MTLDevice
    private let commandQueue: MTLCommandQueue
    private var pStates: [String: MTLComputePipelineState] = [:]
    
    private let shaderSource = """
    #include <metal_stdlib>
    using namespace metal;

    struct LDPCParams {
        uint32_t num_bits;
        uint32_t num_chunks;
        uint32_t g0_words_per_row;
        uint32_t num_full_loops;
        uint32_t remainder;
    };

    // --- v1: 暴力全量版 ---
    kernel void ldpc_v1(
        device const uint32_t* g_matrix [[ buffer(0) ]], 
        device const uint32_t* bit_in   [[ buffer(2) ]], 
        device uint8_t* bit_out         [[ buffer(3) ]], 
        constant LDPCParams& params      [[ buffer(4) ]],
        uint tid [[ thread_position_in_grid ]]) 
    {
        if (tid >= params.num_bits) return;
        uint g_row_offset = tid * params.num_chunks;
        uint32_t parity = 0;
        for (uint j = 0; j < params.num_chunks; j++) {
            parity ^= (popcount(g_matrix[g_row_offset + j] & bit_in[j]) & 1);
        }
        bit_out[tid] = (uint8_t)(parity & 1);
    }

    // --- v3_no_sync: 导航版 (去掉同步) ---
    kernel void ldpc_v3_no_sync(
        device const uint32_t* g_matrix  [[ buffer(0) ]], 
        device const uint32_t* g0_matrix [[ buffer(1) ]], 
        device const uint32_t* bit_in    [[ buffer(2) ]], 
        device uint8_t* bit_out          [[ buffer(3) ]], 
        constant LDPCParams& params       [[ buffer(4) ]],
        uint tid [[ thread_position_in_grid ]]) 
    {
        if (tid >= params.num_bits) return;
        uint g_row_offset = tid * params.num_chunks;
        uint g0_row_offset = tid * params.g0_words_per_row;
        uint32_t parity = 0;
        for (uint i = 0; i < params.g0_words_per_row; i++) {
            uint32_t mask = g0_matrix[g0_row_offset + i];
            while (mask != 0) {
                uint bit_pos = ctz(mask); 
                uint j = (i << 5) + bit_pos; 
                parity ^= (popcount(g_matrix[g_row_offset + j] & bit_in[j]) & 1);
                mask &= ~(1u << bit_pos);
            }
        }
        bit_out[tid] = (uint8_t)(parity & 1);
    }

    // --- v4: 战术小组版 (32线程协作 + SIMD归约) ---
    kernel void ldpc_v4(
        device const uint32_t* g_matrix    [[ buffer(0) ]], 
        constant uint32_t* bit_in          [[ buffer(2) ]], 
        device uint8_t* bit_out            [[ buffer(3) ]], 
        constant LDPCParams&   params      [[ buffer(4) ]],
        uint tid [[ thread_index_in_threadgroup ]],
        uint gid [[ threadgroup_position_in_grid ]])
    {
        uint row = gid;
        if (row >= params.num_bits) return;

        uint g_row_offset = row * params.num_chunks;
        uint32_t partial_parity = 0;

        // 全员循环
        for (uint i = 0; i < params.num_full_loops; i++) {
            uint j = tid + (i << 5); 
            partial_parity ^= popcount(g_matrix[g_row_offset + j] & bit_in[j]);
        }
        // 精准收尾
        if (tid < params.remainder) {
            uint j = tid + (params.num_full_loops << 5);
            partial_parity ^= popcount(g_matrix[g_row_offset + j] & bit_in[j]);
        }

        // 硬件级跨线程 XOR 汇总
        uint32_t final_parity = simd_xor(partial_parity);

        if (tid == 0) {
            bit_out[row] = (uint8_t)(final_parity & 1);
        }
    }
    """

    init(device: MTLDevice) throws {
        self.device = device
        self.commandQueue = device.makeCommandQueue()!
        let library = try device.makeLibrary(source: shaderSource, options: nil)
        for name in ["ldpc_v1", "ldpc_v3_no_sync", "ldpc_v4"] {
            let fn = library.makeFunction(name: name)!
            pStates[name] = try device.makeComputePipelineState(function: fn)
        }
    }

    func benchmark(version: String, g: MTLBuffer, g0: MTLBuffer, input: MTLBuffer, output: MTLBuffer, params: LDPCParams, iterations: Int) -> Double {
        var p = params
        let state = pStates[version]!
        
        for _ in 0..<20 { // 预热次数增加
            runEncoder(version: version, state: state, g: g, g0: g0, input: input, output: output, p: &p, wait: true)
        }
        
        let start = CACurrentMediaTime()
        for _ in 0..<iterations {
            runEncoder(version: version, state: state, g: g, g0: g0, input: input, output: output, p: &p, wait: true)
        }
        
        let cb = commandQueue.makeCommandBuffer()!
        cb.commit()
        cb.waitUntilCompleted()
        let end = CACurrentMediaTime()
        
        return (end - start) * 1000.0 / Double(iterations)
    }

    private func runEncoder(version: String, state: MTLComputePipelineState, g: MTLBuffer, g0: MTLBuffer, input: MTLBuffer, output: MTLBuffer, p: inout LDPCParams, wait: Bool) {
        guard let cb = commandQueue.makeCommandBuffer(),
              let enc = cb.makeComputeCommandEncoder() else { return }
        enc.setComputePipelineState(state)
        enc.setBuffer(g, offset: 0, index: 0)
        enc.setBuffer(g0, offset: 0, index: 1)
        enc.setBuffer(input, offset: 0, index: 2)
        enc.setBuffer(output, offset: 0, index: 3)
        enc.setBytes(&p, length: MemoryLayout<LDPCParams>.size, index: 4)
        
        if version == "ldpc_v4" {
            // v4 专用布局：1个比特对应1个32线程组
            let threadsPerGroup = 32
            let gridSize = MTLSize(width: Int(p.num_bits), height: 1, depth: 1)
            let groupSize = MTLSize(width: threadsPerGroup, height: 1, depth: 1)
            enc.dispatchThreadgroups(gridSize, threadsPerThreadgroup: groupSize)
        } else {
            // v1, v3 专用布局：1个比特对应1个线程
            let threadsPerGroup = 128
            let gridSize = MTLSize(width: (Int(p.num_bits) + threadsPerGroup - 1) / threadsPerGroup * threadsPerGroup, height: 1, depth: 1)
            let groupSize = MTLSize(width: threadsPerGroup, height: 1, depth: 1)
            enc.dispatchThreadgroups(gridSize, threadsPerThreadgroup: groupSize)
        }
        
        enc.endEncoding()
        cb.commit()
        if wait { cb.waitUntilCompleted() }
    }
}
