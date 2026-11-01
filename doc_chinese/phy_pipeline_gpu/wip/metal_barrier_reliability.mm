// 最小 Metal 屏障测试：同一个 buffer、两个 kernel、中间一个设备级内存屏障。
//
// 目的：直接回答"Apple silicon 上 memoryBarrierWithScope:MTLBarrierScopeBuffers 是否可靠"。
// kernel A 把一个递增模式写进 buffer，屏障之后 kernel B 读回来与期望比较，不一致就把计数加一。
// 每轮在 A 与 B 之间可以插入不同长度的延迟，用来模拟 M2 与 M4 的时序差异。
//
// 编译：clang -fobjc-arc -framework Metal -framework Foundation barrier_test.m -o barrier_test
// 运行：./barrier_test <延迟微秒> <轮数>
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
        const unsigned delay_us = (argc > 1) ? (unsigned)atoi(argv[1]) : 0;
        const unsigned rounds   = (argc > 2) ? (unsigned)atoi(argv[2]) : 200;
        const NSUInteger count  = 4096;

        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (dev == nil) { printf("no Metal device\n"); return 2; }
        printf("device: %s  delay=%uus rounds=%u\n", [[dev name] UTF8String], delay_us, rounds);

        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&err];
        if (lib == nil) { printf("compile failed: %s\n", [[err description] UTF8String]); return 2; }
        id<MTLComputePipelineState> pw = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"writer"] error:&err];
        id<MTLComputePipelineState> pr = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"reader"] error:&err];
        if (pw == nil || pr == nil) { printf("pipeline failed\n"); return 2; }

        id<MTLBuffer> buf = [dev newBufferWithLength:count * sizeof(uint32_t) options:MTLResourceStorageModeShared];
        id<MTLBuffer> bad = [dev newBufferWithLength:sizeof(uint32_t) options:MTLResourceStorageModeShared];
        uint32_t *badp = (uint32_t *)bad.contents;

        // 故意只创建 ONE command buffer 和 ONE encoder，A 与 B 在同一个 encoder 里背靠背编码，
        // 彼此之间只隔一个 memoryBarrierWithScope —— 这正是引擎假设的那种用法。
        unsigned total_bad = 0;
        for (unsigned r = 0; r < rounds; ++r) {
            *badp = 0;
            uint32_t seed_a = r * 2 + 1;
            uint32_t seed_b = r * 2 + 1;   // B 期望看到的种子与 A 写入时相同

            id<MTLCommandQueue> q = [dev newCommandQueue];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];

            [enc setComputePipelineState:pw];
            [enc setBuffer:buf offset:0 atIndex:0];
            [enc setBytes:&seed_a length:sizeof(seed_a) atIndex:1];
            [enc dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];

            [enc memoryBarrierWithScope:MTLBarrierScopeBuffers];

            [enc setComputePipelineState:pr];
            [enc setBuffer:buf offset:0 atIndex:0];
            [enc setBytes:&seed_b length:sizeof(seed_b) atIndex:1];
            [enc setBuffer:bad offset:0 atIndex:2];
            [enc dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];

            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];

            if (*badp != 0) { ++total_bad; }
            if (delay_us != 0 && r + 1 < rounds) { usleep(delay_us); }
        }
        printf("mismatched rounds: %u / %u  (%s)\n", total_bad, rounds,
               total_bad == 0 ? "barrier held every time" : "BARRIER DID NOT HOLD");
        return total_bad == 0 ? 0 : 1;
    }
}
