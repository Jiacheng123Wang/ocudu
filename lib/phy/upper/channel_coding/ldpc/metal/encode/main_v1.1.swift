import Metal
import Foundation

// --- 辅助工具：字节内位反转 ---
// 因为 srsRAN 是 MSB-first，而 Metal 映射是 LSB-first
func reverseBitsInByte(_ n: UInt8) -> UInt8 {
    var b = n
    b = (b & 0xF0) >> 4 | (b & 0x0F) << 4
    b = (b & 0xCC) >> 2 | (b & 0x33) << 2
    b = (b & 0xAA) >> 1 | (b & 0x55) << 1
    return b
}

// --- 初始化 Metal 设备 ---
guard let device = MTLCreateSystemDefaultDevice() else {
    fatalError("此设备不支持 Metal")
}

// --- 1. 参数定义 ---
let z = 4
let numBits = 46 * z             // 校验位总数: 184
let numChunks = (22 * z + 31) / 32 // 输入 Words 数: 3
let pageSize = Int(getpagesize())  // Apple Silicon 通常是 16KB 或 4KB

// --- 2. 准备 G 矩阵 (零拷贝加载) ---
let gMatrixPath = "BG1_LSindex0_Z4_G.bin"
let gData = try! Data(contentsOf: URL(fileURLWithPath: gMatrixPath))
let gAlignedSize = (gData.count + pageSize - 1) & ~(pageSize - 1)

var gRawPtr: UnsafeMutableRawPointer?
posix_memalign(&gRawPtr, pageSize, gAlignedSize)
gData.copyBytes(to: gRawPtr!.assumingMemoryBound(to: UInt8.self), count: gData.count)

let gBuffer = device.makeBuffer(bytesNoCopy: gRawPtr!,
                                length: gAlignedSize,
                                options: .storageModeShared,
                                deallocator: { ptr, _ in free(ptr) })!

// --- 3. 准备随机数输入 (零拷贝分配) ---
let inputBytes: [UInt8] = [0xa1, 0x61, 0x53, 0x89, 0x71, 0x1a, 0xaa, 0x42, 0x47, 0x1d, 0x0c]
let inAlignedSize = (numChunks * 4 + pageSize - 1) & ~(pageSize - 1)

var inRawPtr: UnsafeMutableRawPointer?
posix_memalign(&inRawPtr, pageSize, inAlignedSize)
let inWordsPtr = inRawPtr!.assumingMemoryBound(to: UInt32.self)

// 执行位反转并填充 (直接操作指针，无中间拷贝)
let reversedBytes = inputBytes.map { reverseBitsInByte($0) }
// 将反转后的字节流拷贝到对齐后的内存中
reversedBytes.withUnsafeBytes { rawBuffer in
    let dest = UnsafeMutableRawPointer(inWordsPtr)
    dest.copyMemory(from: rawBuffer.baseAddress!, byteCount: reversedBytes.count)
}

let inBuffer = device.makeBuffer(bytesNoCopy: inRawPtr!,
                                 length: inAlignedSize,
                                 options: .storageModeShared,
                                 deallocator: { ptr, _ in free(ptr) })!

// --- 4. 准备输出 (零拷贝分配) ---
let outAlignedSize = (numBits + pageSize - 1) & ~(pageSize - 1)
var outRawPtr: UnsafeMutableRawPointer?
posix_memalign(&outRawPtr, pageSize, outAlignedSize)

let outBuffer = device.makeBuffer(bytesNoCopy: outRawPtr!,
                                  length: outAlignedSize,
                                  options: .storageModeShared,
                                  deallocator: { ptr, _ in free(ptr) })!

// --- 5. 执行编码 ---
do {
    let encoder = try LDPCEncoder(device: device)
    encoder.encode(gBuffer: gBuffer, 
                   inBuffer: inBuffer, 
                   outBuffer: outBuffer, 
                   numBits: numBits, 
                   numChunks: numChunks)
    
    // --- 6. 打印结果 (直接读取零拷贝指针) ---
    let result = outRawPtr!.assumingMemoryBound(to: UInt8.self)
    
    print("--- LDPC Encoding Result (Z=\(z)) ---")
    for r in 0..<30 {
        let startIdx = r * z
        var bits: [UInt8] = []
        var hexVal: UInt8 = 0
        
        for i in 0..<z {
            let bit = result[startIdx + i]
            bits.append(bit)
            // 匹配 srsRAN 格式：第一个 bit 在字节高位
            if bit == 1 {
                hexVal |= (1 << (7 - i))
            }
        }
        
        let hexStr = String(format: "%02x", hexVal)
        print("Row \(String(format: "%2d", r)) | Hex: \(hexStr) | Binary: \(bits)")
    }
} catch {
    print("编码器运行失败: \(error)")
}
