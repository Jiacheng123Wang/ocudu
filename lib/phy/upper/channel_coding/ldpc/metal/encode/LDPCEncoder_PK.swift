import Metal
import Foundation
import QuartzCore

struct LDPCParams {
    var num_bits: UInt32
    var num_chunks: UInt32
    var g0_words_per_row: UInt32
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
    };

    // --- v1: 暴力版 (无同步，全量读取) ---
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
        bit_out[tid] = (uint8_t)parity;
    }

    // --- v2: 极速版 (Threadgroup 缓存同步) ---
    kernel void ldpc_v2(
        device const uint32_t* g_matrix [[ buffer(0) ]], 
        device const uint32_t* bit_in   [[ buffer(2) ]], 
        device uint8_t* bit_out         [[ buffer(3) ]], 
        constant LDPCParams& params      [[ buffer(4) ]],
        uint tid [[ thread_position_in_grid ]],
        uint tgid [[ thread_index_in_threadgroup ]],
        uint tgs  [[ threads_per_threadgroup ]]) 
    {
        threadgroup uint32_t shared_input[264];
        for (uint i = tgid; i < params.num_chunks; i += tgs) {
            shared_input[i] = bit_in[i];
        }
        threadgroup_barrier(mem_flags::mem_threadgroup); // <-- 同步点 1

        if (tid >= params.num_bits) return;
        uint g_row_offset = tid * params.num_chunks;
        uint32_t parity = 0;
        for (uint j = 0; j < params.num_chunks; j++) {
            parity ^= (popcount(g_matrix[g_row_offset + j] & shared_input[j]) & 1);
        }
        bit_out[tid] = (uint8_t)parity;
    }

    // --- v3: 极速版 (G0 导航地图 + While 分歧) ---
    kernel void ldpc_v3(
        device const uint32_t* g_matrix [[ buffer(0) ]], 
        device const uint32_t* g0_matrix [[ buffer(1) ]], 
        device const uint32_t* bit_in   [[ buffer(2) ]], 
        device uint8_t* bit_out         [[ buffer(3) ]], 
        constant LDPCParams& params      [[ buffer(4) ]],
        uint tid [[ thread_position_in_grid ]],
        uint tgid [[ thread_index_in_threadgroup ]],
        uint tgs  [[ threads_per_threadgroup ]]) 
    {
        threadgroup uint32_t shared_input[264];
        for (uint i = tgid; i < params.num_chunks; i += tgs) {
            shared_input[i] = bit_in[i];
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);

        if (tid >= params.num_bits) return;
        uint g_row_offset = tid * params.num_chunks;
        uint g0_row_offset = tid * params.g0_words_per_row;
        uint32_t parity = 0;

        for (uint i = 0; i < params.g0_words_per_row; i++) {
            uint32_t mask = g0_matrix[g0_row_offset + i];
            while (mask != 0) { // <-- 同步点 2: SIMD Divergence (分歧)
                uint bit_pos = ctz(mask); 
                uint j = (i << 5) + bit_pos; 
                parity ^= (popcount(g_matrix[g_row_offset + j] & shared_input[j]) & 1);
                mask &= ~(1u << bit_pos);
            }
        }
        bit_out[tid] = (uint8_t)parity;
    }
    """

    init(device: MTLDevice) throws {
        self.device = device
        self.commandQueue = device.makeCommandQueue()!
        let library = try device.makeLibrary(source: shaderSource, options: nil)
        for name in ["ldpc_v1", "ldpc_v2", "ldpc_v3"] {
            let fn = library.makeFunction(name: name)!
            pStates[name] = try device.makeComputePipelineState(function: fn)
        }
    }

    func benchmark(version: String, g: MTLBuffer, g0: MTLBuffer, input: MTLBuffer, output: MTLBuffer, params: LDPCParams, iterations: Int) -> Double {
        var p = params
        let state = pStates[version]!
        
        // 预热 (Warmup)
        for _ in 0..<10 {
            execute(state: state, g: g, g0: g0, input: input, output: output, p: &p)
        }
        
        let start = CACurrentMediaTime()
        for _ in 0..<iterations {
            execute(state: state, g: g, g0: g0, input: input, output: output, p: &p, wait: false)
        }
        // 最后一批提交并等待
        guard let lastBuffer = commandQueue.makeCommandBuffer() else { return 0 }
        lastBuffer.commit()
        lastBuffer.waitUntilCompleted()
        let end = CACurrentMediaTime()
        
        return (end - start) * 1000.0 / Double(iterations) // 返回单次平均毫秒数
    }

    private func execute(state: MTLComputePipelineState, g: MTLBuffer, g0: MTLBuffer, input: MTLBuffer, output: MTLBuffer, p: inout LDPCParams, wait: Bool = true) {
        guard let cb = commandQueue.makeCommandBuffer(),
              let enc = cb.makeComputeCommandEncoder() else { return }
        enc.setComputePipelineState(state)
        enc.setBuffer(g, offset: 0, index: 0)
        enc.setBuffer(g0, offset: 0, index: 1)
        enc.setBuffer(input, offset: 0, index: 2)
        enc.setBuffer(output, offset: 0, index: 3)
        enc.setBytes(&p, length: MemoryLayout<LDPCParams>.size, index: 4)
        
        let threadsPerGroup = 128
        let gridSize = MTLSize(width: (Int(p.num_bits) + threadsPerGroup - 1) / threadsPerGroup * threadsPerGroup, height: 1, depth: 1)
        let groupSize = MTLSize(width: threadsPerGroup, height: 1, depth: 1)
        enc.dispatchThreadgroups(gridSize, threadsPerThreadgroup: groupSize)
        enc.endEncoding()
        cb.commit()
        if wait { cb.waitUntilCompleted() }
    }
}
