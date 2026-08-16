#pragma once
#include <cstdint>
#include <cstddef>

/**
 * @brief 通用型 Metal 译码参数结构体
 * 适用于 update_s (Target=s, Source=p) 和 update_p (Target=p, Source=s)
 */
struct DecodeParams {
    uint32_t num_target; // 当前需要更新的目标比特总数 (如 n_info 或 n_parity)
    uint32_t num_source; // 对应的对偶空间比特总数 (如 n_parity 或 n_info)
    
    // --- "比特 -> 校验方程" 映射维度 (对应转置矩阵 GT 或 FT) ---
    // 用于让每个目标比特找到其参与的所有校验方程
    uint32_t b2c_num_chunks; // 每行对应的 uint32 数量
    uint32_t b2c_full_loops; // 32线程全速循环次数 (num_chunks / 32)
    uint32_t b2c_remainder;  // 剩余需要处理的 word 数量 (num_chunks % 32)
    
    // --- "校验方程 -> 比特" 映射维度 (对应原矩阵 G 或 F) ---
    // 用于在特定校验方程中查找其他参与的“同行”比特
    uint32_t c2b_num_chunks; 
    uint32_t c2b_full_loops; 
    uint32_t c2b_remainder;  
    
    uint32_t top_k; // 嫌疑人筛选阈值
    float alpha;    // 步长/松弛因子
};

class MetalDecoderSUpdate {
public:
    MetalDecoderSUpdate();
    ~MetalDecoderSUpdate();

    /**
     * @brief 静态内存管理工具：创建支持统一内存(Shared)的 Metal Buffer
     * @return 返回封装为 void* 的 id<MTLBuffer>，引用计数已递增
     */
    static void* alloc_buffer(void* device_ptr, size_t size);

    /**
     * @brief 静态内存管理工具：释放 void* 指向的 Metal Buffer
     */
    static void free_buffer(void* mtl_buffer_ptr);

    /**
     * @brief 获取底层 Metal 设备句柄，用于 Buffer 申请
     */
    void* get_device_handle();

    // 译码器外层直接调用的执行函数
    // 传入的指针必须是底层 MTLBuffer 对象的包装引用
    void execute_update(
        void* s_llr_mtl_buffer, 
        void* p_llr_mtl_buffer,
        void* p_hard_mtl_buffer, 
        void* p_pred_mtl_buffer,
        void* G_bits_mtl_buffer, 
        void* GT_bits_mtl_buffer,
        void* fixed_part_mtl_buffer,
        const DecodeParams& params
    );

private:
    // 使用 PIMPL 模式隐藏 Objective-C / Metal 具体的实现对象
    struct MetalContext;
    MetalContext* ctx;
};