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

// 1. 设置参数 (Z=256)
let z = 256
let numRows = 46                 // 总行数
let numBits = numRows * z         // 总编码比特: 11776
let numChunks = (22 * z + 31) / 32 // G 矩阵每行 word 数: 176
let g0WordsPerRow = (numChunks + 31) / 32 // G0 每行 word 数: 6

let gFile = "BG1_LSindex0_Z256_G.bin"
let g0File = "BG1_LSindex0_Z256_G0.bin"

// 2. 加载 G 矩阵 (零拷贝)
let gData = try! Data(contentsOf: URL(fileURLWithPath: gFile))
let gAlignedSize = (gData.count + pageSize - 1) & ~(pageSize - 1)
var gRawPtr: UnsafeMutableRawPointer?
posix_memalign(&gRawPtr, pageSize, gAlignedSize)
gData.copyBytes(to: gRawPtr!.assumingMemoryBound(to: UInt8.self), count: gData.count)
let gBuffer = device.makeBuffer(bytesNoCopy: gRawPtr!, length: gAlignedSize, options: .storageModeShared, deallocator: { ptr, _ in free(ptr) })!

// 3. 加载 G0 矩阵 (从文件读取，零拷贝)
let g0Data = try! Data(contentsOf: URL(fileURLWithPath: g0File))
let g0AlignedSize = (g0Data.count + pageSize - 1) & ~(pageSize - 1)
var g0RawPtr: UnsafeMutableRawPointer?
posix_memalign(&g0RawPtr, pageSize, g0AlignedSize)
g0Data.copyBytes(to: g0RawPtr!.assumingMemoryBound(to: UInt8.self), count: g0Data.count)
let g0Buffer = device.makeBuffer(bytesNoCopy: g0RawPtr!, length: g0AlignedSize, options: .storageModeShared, deallocator: { ptr, _ in free(ptr) })!

// 4. 准备输入 bit_in (位反转处理)
let inputBytes = parseHexInput(path: "input_z256.txt")
let inAlignedSize = (numChunks * 4 + pageSize - 1) & ~(pageSize - 1)
var inRawPtr: UnsafeMutableRawPointer?
posix_memalign(&inRawPtr, pageSize, inAlignedSize)
let reversedInput = inputBytes.map { reverseBitsInByte($0) }
reversedInput.withUnsafeBytes { inRawPtr!.copyMemory(from: $0.baseAddress!, byteCount: min(reversedInput.count, inAlignedSize)) }
let inBuffer = device.makeBuffer(bytesNoCopy: inRawPtr!, length: inAlignedSize, options: .storageModeShared, deallocator: { ptr, _ in free(ptr) })!

// 5. 准备输出 Buffer
let outAlignedSize = (numBits + pageSize - 1) & ~(pageSize - 1)
var outRawPtr: UnsafeMutableRawPointer?
posix_memalign(&outRawPtr, pageSize, outAlignedSize)
let outBuffer = device.makeBuffer(bytesNoCopy: outRawPtr!, length: outAlignedSize, options: .storageModeShared, deallocator: { ptr, _ in free(ptr) })!

// 6. 运行编码器
do {
    let encoder = try LDPCEncoder(device: device)
    let params = LDPCParams(num_bits: UInt32(numBits), num_chunks: UInt32(numChunks), g0_words_per_row: UInt32(g0WordsPerRow))
    
    print(">>> 极速版 3.0 启动 (Z=\(z), G0从文件加载)")
    let startTime = CACurrentMediaTime()
    encoder.encode(gBuffer: gBuffer, g0Buffer: g0Buffer, inBuffer: inBuffer, outBuffer: outBuffer, params: params)
    let endTime = CACurrentMediaTime()
    print(">>> GPU 编码完成，耗时: \(String(format: "%.3f", (endTime - startTime)*1000)) ms")
    
    // --- 7. 打印全部 46 行结果 (按照要求的格式) ---
    let result = outRawPtr!.assumingMemoryBound(to: UInt8.self)
    let bytesPerRow = z / 8 // Z=256 时为 32 字节
    
    print("\n--- 编码输出结果 (全部 \(numRows) 行) ---")
    for r in 0..<numRows {
        let rowStartBit = r * z
        var hexStrings: [String] = []
        
        // 将比特流按字节打包
        for b in 0..<bytesPerRow {
            var byteVal: UInt8 = 0
            for i in 0..<8 {
                let bit = result[rowStartBit + b * 8 + i]
                if bit == 1 {
                    // MSB-first 映射：第一个 bit 是最高位
                    byteVal |= (1 << (7 - i))
                }
            }
            hexStrings.append(String(format: "0x%02x", byteVal))
        }
        
        // 格式化输出：Row 行号 | Hex: 数据...
        let rowLabel = String(format: "Row %2d", r)
        let hexContent = hexStrings.joined(separator: " ")
        print("\(rowLabel) | Hex: \(hexContent)")
    }
    
} catch {
    print("发生错误: \(error)")
}
