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
    
    private let shaderSource = """
    #include <metal_stdlib>
    using namespace metal;

    struct LDPCParams {
        uint32_t num_bits;
        uint32_t num_chunks;
    };

    kernel void ldpc_encode_optimized(
        device const uint32_t* g_matrix    [[ buffer(0) ]], 
        device const uint32_t* bit_in      [[ buffer(1) ]], 
        device uint8_t* bit_out            [[ buffer(2) ]], 
        constant LDPCParams&   params      [[ buffer(3) ]],
        uint tid [[ thread_position_in_grid ]]) 
    {
        if (tid >= params.num_bits) return;
        uint row_offset = tid * params.num_chunks;
        uint32_t parity = 0;
        for (uint j = 0; j < params.num_chunks; j++) {
            parity ^= (popcount(g_matrix[row_offset + j] & bit_in[j]) & 1);
        }
        bit_out[tid] = (uint8_t)parity;
    }
    """
    
    init(device: MTLDevice) throws {
        self.device = device
        self.commandQueue = device.makeCommandQueue()!
        let library = try device.makeLibrary(source: shaderSource, options: nil)
        let function = library.makeFunction(name: "ldpc_encode_optimized")!
        self.pipelineState = try device.makeComputePipelineState(function: function)
    }
    
    // 改良版：直接接受 MTLBuffer，外部负责 Buffer 的零拷贝维护
    func encode(gBuffer: MTLBuffer, inBuffer: MTLBuffer, outBuffer: MTLBuffer, numBits: Int, numChunks: Int) {
        var params = LDPCParams(num_bits: UInt32(numBits), num_chunks: UInt32(numChunks))
        guard let commandBuffer = commandQueue.makeCommandBuffer(),
              let encoder = commandBuffer.makeComputeCommandEncoder() else { return }
        
        encoder.setComputePipelineState(pipelineState)
        encoder.setBuffer(gBuffer, offset: 0, index: 0)
        encoder.setBuffer(inBuffer, offset: 0, index: 1)
        encoder.setBuffer(outBuffer, offset: 0, index: 2)
        encoder.setBytes(&params, length: MemoryLayout<LDPCParams>.size, index: 3)
        
        encoder.dispatchThreads(MTLSize(width: numBits, height: 1, depth: 1), 
                               threadsPerThreadgroup: MTLSize(width: min(numBits, 32), height: 1, depth: 1))
        encoder.endEncoding()
        commandBuffer.commit()
        commandBuffer.waitUntilCompleted()
    }
}
