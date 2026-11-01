// 跨提交 + shared buffer 的陈旧读取测试。
//
// 三组对照，都是"kernel A 写一个模式，kernel B 读回来核对"，区别只在同步方式：
//   A: 同一个 command buffer + memoryBarrierWithScope           —— §16 已证明可靠（对照基线）
//   B: 两个 command buffer，中间 waitUntilCompleted（shared buffer）—— 引擎当前的做法
//   C: 两个 command buffer，中间 MTLSharedEvent 的 signal/wait   —— 候选修复
// 另外用 host 写入（memcpy）在两次提交之间改 buffer，模拟"主机改了数据、GPU 却读到旧值"。
//
// 编译：clang -fobjc-arc -framework Metal -framework Foundation cross_submit_test.m -o cross_submit_test
// 运行：./cross_submit_test <模式 A|B|C> <轮数>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

static NSString *const kSrc = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"kernel void writer(device uint* buf [[buffer(0)]], constant uint& seed [[buffer(1)]], uint gid [[thread_position_in_grid]]) {\n"
"  buf[gid] = seed * 2654435761u + gid;\n"
"}\n"
"kernel void reader(device uint* buf [[buffer(0)]], constant uint& seed [[buffer(1)]],\n"
"                   device atomic_uint* bad [[buffer(2)]], uint gid [[thread_position_in_grid]]) {\n"
"  uint expect = seed * 2654435761u + gid;\n"
"  if (buf[gid] != expect) { atomic_fetch_add_explicit(bad, 1u, memory_order_relaxed); }\n"
"}\n";

int main(int argc, char **argv)
{
    @autoreleasepool {
        const char mode = (argc > 1) ? argv[1][0] : 'A';
        const unsigned rounds = (argc > 2) ? (unsigned)atoi(argv[2]) : 200;
        const NSUInteger count = 4096;

        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&err];
        id<MTLComputePipelineState> pw = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"writer"] error:&err];
        id<MTLComputePipelineState> pr = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"reader"] error:&err];
        if (pw == nil || pr == nil) { printf("pipeline failed\n"); return 2; }

        id<MTLBuffer> buf = [dev newBufferWithLength:count * sizeof(uint32_t) options:MTLResourceStorageModeShared];
        id<MTLBuffer> bad = [dev newBufferWithLength:sizeof(uint32_t) options:MTLResourceStorageModeShared];
        uint32_t *badp = (uint32_t *)bad.contents;
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLSharedEvent> ev = [dev newSharedEvent];

        unsigned total_bad = 0;
        for (unsigned r = 0; r < rounds; ++r) {
            *badp = 0;
            uint32_t seed = r * 2 + 1;

            if (mode == 'A') {
                // 同一个 encoder + 设备级屏障
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
                [enc setComputePipelineState:pw];
                [enc setBuffer:buf offset:0 atIndex:0];
                [enc setBytes:&seed length:sizeof(seed) atIndex:1];
                [enc dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
                [enc memoryBarrierWithScope:MTLBarrierScopeBuffers];
                [enc setComputePipelineState:pr];
                [enc setBuffer:buf offset:0 atIndex:0];
                [enc setBytes:&seed length:sizeof(seed) atIndex:1];
                [enc setBuffer:bad offset:0 atIndex:2];
                [enc dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
                [enc endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
            } else {
                // 写：第一个 command buffer
                id<MTLCommandBuffer> cb1 = [q commandBuffer];
                id<MTLComputeCommandEncoder> e1 = [cb1 computeCommandEncoder];
                [e1 setComputePipelineState:pw];
                [e1 setBuffer:buf offset:0 atIndex:0];
                [e1 setBytes:&seed length:sizeof(seed) atIndex:1];
                [e1 dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
                [e1 endEncoding];
                if (mode == 'C') { [cb1 encodeSignalEvent:ev value:r + 1]; }
                [cb1 commit];

                if (mode == 'B') {
                    [cb1 waitUntilCompleted];         // 引擎当前的做法
                } else {
                    [ev waitUntilSignaledValue:r + 1 timeoutMS:1000];   // 候选修复
                }

                // 读：第二个 command buffer
                id<MTLCommandBuffer> cb2 = [q commandBuffer];
                id<MTLComputeCommandEncoder> e2 = [cb2 computeCommandEncoder];
                [e2 setComputePipelineState:pr];
                [e2 setBuffer:buf offset:0 atIndex:0];
                [e2 setBytes:&seed length:sizeof(seed) atIndex:1];
                [e2 setBuffer:bad offset:0 atIndex:2];
                [e2 dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
                [e2 endEncoding];
                [cb2 commit];
                [cb2 waitUntilCompleted];
            }
            if (mode == 'D') {
                // ★ GPU 写 → CPU 读：引擎的真实情形（CE 在 GPU 写网格，均衡在 CPU 读网格）
                id<MTLCommandBuffer> cbw = [q commandBuffer];
                id<MTLComputeCommandEncoder> ew = [cbw computeCommandEncoder];
                [ew setComputePipelineState:pw];
                [ew setBuffer:buf offset:0 atIndex:0];
                [ew setBytes:&seed length:sizeof(seed) atIndex:1];
                [ew dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
                [ew endEncoding];
                [cbw commit];
                [cbw waitUntilCompleted];
                const uint32_t *p = (const uint32_t *)buf.contents;
                for (NSUInteger i = 0; i < count; ++i) {
                    uint32_t expect = seed * 2654435761u + (uint32_t)i;
                    if (p[i] != expect) { ++(*badp); break; }
                }
            } else if (mode == 'E') {
                // ★ CPU 写 → GPU 读：ggml 报告里的那一类（主机 memcpy 之后 GPU 读到旧值）
                uint32_t *p = (uint32_t *)buf.contents;
                for (NSUInteger i = 0; i < count; ++i) { p[i] = seed * 2654435761u + (uint32_t)i; }
                id<MTLCommandBuffer> cbr = [q commandBuffer];
                id<MTLComputeCommandEncoder> er = [cbr computeCommandEncoder];
                [er setComputePipelineState:pr];
                [er setBuffer:buf offset:0 atIndex:0];
                [er setBytes:&seed length:sizeof(seed) atIndex:1];
                [er setBuffer:bad offset:0 atIndex:2];
                [er dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
                [er endEncoding];
                [cbr commit];
                [cbr waitUntilCompleted];
            }
            if (*badp != 0) { ++total_bad; }
        }
        printf("mode %c: mismatched rounds %u / %u  (%s)\n", mode, total_bad, rounds,
               total_bad == 0 ? "no stale read" : "STALE READ OBSERVED");
        return total_bad == 0 ? 0 : 1;
    }
}
