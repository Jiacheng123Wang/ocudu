import Metal
import Foundation
import QuartzCore

// ... 此处省略 reverseBitsInByte, parseHexInput 等工具函数 (见之前代码) ...
// --- 辅助工具：位反转 (srsRAN MSB-first 对齐) ---
func reverseBitsInByte(_ n: UInt8) -> UInt8 {
    var b = n
    b = (b & 0xF0) >> 4 | (b & 0x0F) << 4
    b = (b & 0xCC) >> 2 | (b & 0x33) << 2
    b = (b & 0xAA) >> 1 | (b & 0x55) << 1
    return b
}

// --- 辅助工具：解析十六进制文本文件 ---
func parseHexInput(path: String) -> [UInt8] {
    guard let content = try? String(contentsOfFile: path, encoding: .utf8) else {
        fatalError("无法读取输入文件: \(path)")
    }
    let cleanedContent = content.replacingOccurrences(of: "0x", with: "")
    let components = cleanedContent.components(separatedBy: CharacterSet(charactersIn: ", \n\r\t"))
    return components.compactMap { comp in
        let trimmed = comp.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty { return nil }
        return UInt8(trimmed, radix: 16)
    }
}


guard let device = MTLCreateSystemDefaultDevice() else { fatalError() }
let pageSize = Int(getpagesize())

// 1. 加载所有数据 (G, G0, Input)
let z = 256
let numBits = 46 * z
let numChunks = (22 * z + 31) / 32 // 176
let g0WordsPerRow = (numChunks + 31) / 32 // 6

// (请确保 bin 文件在当前目录下)
func makeAlignedBuffer(size: Int) -> MTLBuffer {
    let alignedSize = (size + pageSize - 1) & ~(pageSize - 1)
    var ptr: UnsafeMutableRawPointer?
    posix_memalign(&ptr, pageSize, alignedSize)
    return device.makeBuffer(bytesNoCopy: ptr!, length: alignedSize, options: .storageModeShared, deallocator: { p, _ in free(p) })!
}

let gBuffer = makeAlignedBuffer(size: numBits * numChunks * 4)
let g0Buffer = makeAlignedBuffer(size: numBits * g0WordsPerRow * 4)
let inBuffer = makeAlignedBuffer(size: numChunks * 4)
let outBuffer = makeAlignedBuffer(size: numBits)

// 加载数据略... (请自行补充 Data(contentsOf:) 到 Buffer 的逻辑)

do {
    let encoder = try LDPCEncoder(device: device)
    let params = LDPCParams(num_bits: UInt32(numBits), num_chunks: UInt32(numChunks), g0_words_per_row: UInt32(g0WordsPerRow))
    
    print(">>> 启动对比测试 (1000次压力测试, Z=\(z))")
    
    let timeV1 = encoder.benchmark(version: "ldpc_v1", g: gBuffer, g0: g0Buffer, input: inBuffer, output: outBuffer, params: params, iterations: 10000)
    print("v1 暴力版平均耗时: \(String(format: "%.4f", timeV1)) ms")

    let timeV2 = encoder.benchmark(version: "ldpc_v2", g: gBuffer, g0: g0Buffer, input: inBuffer, output: outBuffer, params: params, iterations: 10000)
    print("v2 缓存版平均耗时: \(String(format: "%.4f", timeV2)) ms")

    let timeV3 = encoder.benchmark(version: "ldpc_v3", g: gBuffer, g0: g0Buffer, input: inBuffer, output: outBuffer, params: params, iterations: 10000)
    print("v3 导航版平均耗时: \(String(format: "%.4f", timeV3)) ms")
    
    print("--------------------------------------")
    print("加速比 (v3 vs v1): \(String(format: "%.2f", timeV1/timeV3))x")
} catch {
    print(error)
}
