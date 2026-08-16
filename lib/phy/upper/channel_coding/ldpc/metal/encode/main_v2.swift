import Metal
import Foundation

// --- 辅助工具：字节内位反转 (srsRAN MSB-first 对齐) ---
func reverseBitsInByte(_ n: UInt8) -> UInt8 {
    var b = n
    b = (b & 0xF0) >> 4 | (b & 0x0F) << 4
    b = (b & 0xCC) >> 2 | (b & 0x33) << 2
    b = (b & 0xAA) >> 1 | (b & 0x55) << 1
    return b
}

// --- 辅助工具：解析十六进制文本文件 ---
func parseHexInput(path: String) -> [UInt8] {
    // 修正警告：添加 encoding: .utf8
    guard let content = try? String(contentsOfFile: path, encoding: .utf8) else {
        fatalError("无法读取输入文件: \(path)")
    }
    
    // 使用更健壮的正则表达式或字符集分割，过滤掉逗号、换行、空格和 0x
    let cleanedContent = content.replacingOccurrences(of: "0x", with: "")
    let components = cleanedContent.components(separatedBy: CharacterSet(charactersIn: ", \n\r\t"))
    
    return components.compactMap { comp in
        let trimmed = comp.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty { return nil }
        return UInt8(trimmed, radix: 16)
    }
}

// --- 初始化 Metal ---
guard let device = MTLCreateSystemDefaultDevice() else { fatalError() }

// --- 1. 参数定义 (Z=256) ---
let z = 256
let numBits = 46 * z              // 11776
let numChunks = (22 * z + 31) / 32  // 176 (5632 bits)
let pageSize = Int(getpagesize())

// --- 2. 准备 G 矩阵 (零拷贝) ---
let gMatrixPath = "BG1_LSindex0_Z256_G.bin"
let gData = try! Data(contentsOf: URL(fileURLWithPath: gMatrixPath))
let gAlignedSize = (gData.count + pageSize - 1) & ~(pageSize - 1)
var gRawPtr: UnsafeMutableRawPointer?
posix_memalign(&gRawPtr, pageSize, gAlignedSize)
gData.copyBytes(to: gRawPtr!.assumingMemoryBound(to: UInt8.self), count: gData.count)

let gBuffer = device.makeBuffer(bytesNoCopy: gRawPtr!, length: gAlignedSize, options: .storageModeShared, deallocator: { ptr, _ in free(ptr) })!

// --- 3. 准备随机数输入 (从文本读取并位反转) ---
let inputBytes = parseHexInput(path: "input_z256.txt")
let inAlignedSize = (numChunks * 4 + pageSize - 1) & ~(pageSize - 1)
var inRawPtr: UnsafeMutableRawPointer?
posix_memalign(&inRawPtr, pageSize, inAlignedSize)
let inWordsPtr = inRawPtr!.assumingMemoryBound(to: UInt32.self)

// 执行位反转并填充 (确保 LSB-first 内存布局)
let reversedBytes = inputBytes.map { reverseBitsInByte($0) }
reversedBytes.withUnsafeBytes { rawBuf in
    let dest = UnsafeMutableRawPointer(inWordsPtr)
    dest.copyMemory(from: rawBuf.baseAddress!, byteCount: min(reversedBytes.count, inAlignedSize))
}

let inBuffer = device.makeBuffer(bytesNoCopy: inRawPtr!, length: inAlignedSize, options: .storageModeShared, deallocator: { ptr, _ in free(ptr) })!

// --- 4. 准备输出 (零拷贝) ---
let outAlignedSize = (numBits + pageSize - 1) & ~(pageSize - 1)
var outRawPtr: UnsafeMutableRawPointer?
posix_memalign(&outRawPtr, pageSize, outAlignedSize)
let outBuffer = device.makeBuffer(bytesNoCopy: outRawPtr!, length: outAlignedSize, options: .storageModeShared, deallocator: { ptr, _ in free(ptr) })!

// --- 5. 执行编码 ---
do {
    let encoder = try LDPCEncoder(device: device)
    encoder.encode(gBuffer: gBuffer, inBuffer: inBuffer, outBuffer: outBuffer, numBits: numBits, numChunks: numChunks)
    
    // --- 6. 按照要求格式打印结果 ---
    let result = outRawPtr!.assumingMemoryBound(to: UInt8.self)
    print("--- LDPC 大容量测试 (Z=\(z)) ---")
    
    for r in 0..<46 {
        let startBitIdx = r * z
        var hexOutputs: [String] = []
        
        // 每行打印前 10 个 Byte (80 bit)
        for byteIdx in 0..<10 {
            var currentByte: UInt8 = 0
            for bitOffset in 0..<8 {
                let bit = result[startBitIdx + byteIdx * 8 + bitOffset]
                if bit == 1 {
                    // srsRAN 习惯：第一个 bit 在字节最高位 (MSB)
                    currentByte |= (1 << (7 - bitOffset))
                }
            }
            hexOutputs.append(String(format: "0x%02x", currentByte))
        }
        
        let rowStr = String(format: "%2d", r)
        print("Row \(rowStr) | Hex: \(hexOutputs.joined(separator: " ")) ...")
    }
} catch {
    print("错误: \(error)")
}
