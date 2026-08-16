import Metal
import Foundation
import QuartzCore

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

// --- 执行流程 ---
guard let device = MTLCreateSystemDefaultDevice() else { fatalError("未找到 GPU 设备") }
let pageSize = Int(getpagesize())

// 1. 设置参数 (可根据需要修改 Z)
let z = 256
let numRows = 46                 
let numBits = numRows * z         
let numChunks = (22 * z + 31) / 32 // Z=256 时为 176

// --- 4.0 预计算参数 ---
let numFullLoops = UInt32(numChunks / 32)
let remainder = UInt32(numChunks % 32)

let gFile = "BG1_LSindex0_Z256_G.bin"

// 2. 加载 G 矩阵 (零拷贝)
let gData = try! Data(contentsOf: URL(fileURLWithPath: gFile))
let gAlignedSize = (gData.count + pageSize - 1) & ~(pageSize - 1)
var gRawPtr: UnsafeMutableRawPointer?
posix_memalign(&gRawPtr, pageSize, gAlignedSize)
gData.copyBytes(to: gRawPtr!.assumingMemoryBound(to: UInt8.self), count: gData.count)
let gBuffer = device.makeBuffer(bytesNoCopy: gRawPtr!, length: gAlignedSize, options: .storageModeShared, deallocator: { ptr, _ in free(ptr) })!

// 3. 准备输入 bit_in (位反转处理)
let inputBytes = parseHexInput(path: "input_z256.txt")
let inAlignedSize = (numChunks * 4 + pageSize - 1) & ~(pageSize - 1)
var inRawPtr: UnsafeMutableRawPointer?
posix_memalign(&inRawPtr, pageSize, inAlignedSize)
let reversedInput = inputBytes.map { reverseBitsInByte($0) }
reversedInput.withUnsafeBytes { inRawPtr!.copyMemory(from: $0.baseAddress!, byteCount: min(reversedInput.count, inAlignedSize)) }
let inBuffer = device.makeBuffer(bytesNoCopy: inRawPtr!, length: inAlignedSize, options: .storageModeShared, deallocator: { ptr, _ in free(ptr) })!

// 4. 准备输出 Buffer
let outAlignedSize = (numBits + pageSize - 1) & ~(pageSize - 1)
var outRawPtr: UnsafeMutableRawPointer?
posix_memalign(&outRawPtr, pageSize, outAlignedSize)
let outBuffer = device.makeBuffer(bytesNoCopy: outRawPtr!, length: outAlignedSize, options: .storageModeShared, deallocator: { ptr, _ in free(ptr) })!

// 5. 运行编码器
do {
    let encoder = try LDPCEncoder(device: device)
    let params = LDPCParams(
        num_bits: UInt32(numBits), 
        num_chunks: UInt32(numChunks),
        num_full_loops: numFullLoops,
        remainder: remainder
    )
    
    print(">>> 极速版 4.0 启动 (Z=\(z), SIMD-Group Collaboration)")
    print(">>> 运行配置: Chunks=\(numChunks), FullLoops=\(numFullLoops), Remainder=\(remainder)")
    
    let startTime = CACurrentMediaTime()
    // v4 版本的接口已经去掉了 g0Buffer
    encoder.encode(gBuffer: gBuffer, inBuffer: inBuffer, outBuffer: outBuffer, params: params)
    let endTime = CACurrentMediaTime()
    print(">>> GPU 编码完成，耗时: \(String(format: "%.4f", (endTime - startTime)*1000)) ms")
    
    // --- 6. 打印全部 46 行结果进行对齐验证 ---
    let result = outRawPtr!.assumingMemoryBound(to: UInt8.self)
    //let bytesPerRow = z / 8
    let bytesPerRow = 10 
    
    print("\n--- 编码输出结果 (全部 \(numRows) 行) ---")
    for r in 0..<numRows {
        let rowStartBit = r * z
        var hexStrings: [String] = []
        for b in 0..<bytesPerRow {
            var byteVal: UInt8 = 0
            for i in 0..<8 {
                let bit = result[rowStartBit + b * 8 + i]
                if bit == 1 {
                    byteVal |= (1 << (7 - i))
                }
            }
            hexStrings.append(String(format: "0x%02x", byteVal))
        }
        print(String(format: "Row %2d | Hex: ", r) + hexStrings.joined(separator: " "))
    }
    
} catch {
    print("发生错误: \(error)")
}
