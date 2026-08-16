import Metal
import Foundation
import QuartzCore

// --- 辅助工具 ---
func reverseBitsInByte(_ n: UInt8) -> UInt8 {
    var b = n
    b = (b & 0xF0) >> 4 | (b & 0x0F) << 4
    b = (b & 0xCC) >> 2 | (b & 0x33) << 2
    b = (b & 0xAA) >> 1 | (b & 0x55) << 1
    return b
}

func parseHexInput(path: String) -> [UInt8] {
    guard let content = try? String(contentsOfFile: path, encoding: .utf8) else { return [] }
    let cleaned = content.replacingOccurrences(of: "0x", with: "")
    let components = cleaned.components(separatedBy: CharacterSet(charactersIn: ", \n\r\t"))
    return components.compactMap { UInt8($0.trimmingCharacters(in: .whitespaces), radix: 16) }
}

// --- 初始化 ---
guard let device = MTLCreateSystemDefaultDevice() else { fatalError() }
let pageSize = Int(getpagesize())

let z = 256
let numBits = 46 * z
let numChunks = (22 * z + 31) / 32 // 176
let g0WordsPerRow = (numChunks + 31) / 32 // 6

func makeAlignedBuffer(size: Int) -> MTLBuffer {
    let alignedSize = (size + pageSize - 1) & ~(pageSize - 1)
    var ptr: UnsafeMutableRawPointer?
    posix_memalign(&ptr, pageSize, alignedSize)
    return device.makeBuffer(bytesNoCopy: ptr!, length: alignedSize, options: .storageModeShared, deallocator: { p, _ in free(p) })!
}

// 1. 准备 Buffer
let gBuffer = makeAlignedBuffer(size: numBits * numChunks * 4)
let g0Buffer = makeAlignedBuffer(size: numBits * g0WordsPerRow * 4)
let inBuffer = makeAlignedBuffer(size: numChunks * 4)
let outBuffer = makeAlignedBuffer(size: numBits)

// 2. 加载数据
let gFile = "BG1_LSindex0_Z256_G.bin"
let g0File = "BG1_LSindex0_Z256_G0.bin"
let inFile = "input_z256.txt"

if let gd = try? Data(contentsOf: URL(fileURLWithPath: gFile)) {
    gd.copyBytes(to: gBuffer.contents().assumingMemoryBound(to: UInt8.self), count: min(gd.count, gBuffer.length))
}
if let g0d = try? Data(contentsOf: URL(fileURLWithPath: g0File)) {
    g0d.copyBytes(to: g0Buffer.contents().assumingMemoryBound(to: UInt8.self), count: min(g0d.count, g0Buffer.length))
}
let inputBytes = parseHexInput(path: inFile)
let reversedInput = inputBytes.map { reverseBitsInByte($0) }
reversedInput.withUnsafeBytes { inBuffer.contents().copyMemory(from: $0.baseAddress!, byteCount: min(reversedInput.count, inBuffer.length)) }

// 3. 执行 Benchmark
do {
    let encoder = try LDPCEncoder(device: device)
    let params = LDPCParams(num_bits: UInt32(numBits), num_chunks: UInt32(numChunks), g0_words_per_row: UInt32(g0WordsPerRow))
    
    print(">>> 开始 LDPC 多架构对比测试 (1000次压力测试, Z=\(z))")
    print("---------------------------------------------------------")
    
    let v1 = encoder.benchmark(version: "ldpc_v1", g: gBuffer, g0: g0Buffer, input: inBuffer, output: outBuffer, params: params, iterations: 10000)
    print("V1 暴力全量版:    \(String(format: "%.4f", v1)) ms")

    let v2 = encoder.benchmark(version: "ldpc_v2", g: gBuffer, g0: g0Buffer, input: inBuffer, output: outBuffer, params: params, iterations: 10000)
    print("V2 缓存同步版:    \(String(format: "%.4f", v2)) ms")

    let v3 = encoder.benchmark(version: "ldpc_v3", g: gBuffer, g0: g0Buffer, input: inBuffer, output: outBuffer, params: params, iterations: 10000)
    print("V3 导航同步版:    \(String(format: "%.4f", v3)) ms")
    
    let v3ns = encoder.benchmark(version: "ldpc_v3_no_sync", g: gBuffer, g0: g0Buffer, input: inBuffer, output: outBuffer, params: params, iterations: 10000)
    print("V3_NoSync 导航无同步版: \(String(format: "%.4f", v3ns)) ms")
    
    print("---------------------------------------------------------")
    print("M2 结论预期: V3 应该最快。")
    print("M4 Pro 结论预期: 请观察 V1 与 V3_NoSync 的巅峰对决。")
} catch {
    print("错误: \(error)")
}
