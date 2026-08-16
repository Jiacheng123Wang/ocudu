#ifndef METAL_ALIGNED_BUFFER_HPP
#define METAL_ALIGNED_BUFFER_HPP

#include <iostream>
#include <cstdlib>
#include <unistd.h>
#include <algorithm>
#include <stdexcept>
#include <cstring>

/**
 * @brief 专为 Metal 零拷贝设计的对齐内存管理器
 * 确保内存按系统页大小 (4KB) 对齐，满足 MTLDevice newBufferWithBytesNoCopy 的要求
 */
template <typename T>
class MetalAlignedBuffer {
private:
    T* data_ptr = nullptr;
    size_t logic_count = 0;
    size_t physical_bytes_size = 0;

public:
    /**
     * @brief 构造函数
     * @param engine 传入 MetalEngine 句柄（匹配 DecoderContext 的调用需求）
     * @param count 需要存储的元素数量
     */
    MetalAlignedBuffer(void* engine, size_t count) : logic_count(count) {
        if (count == 0) {
            data_ptr = nullptr;
            physical_bytes_size = 0;
            return;
        }

        size_t page_size = sysconf(_SC_PAGESIZE); 
        size_t raw_size = count * sizeof(T);
        
        // 1. 计算物理对齐大小：向上取整到 page_size 的倍数
        physical_bytes_size = (raw_size + page_size - 1) & ~(page_size - 1);
        
        // 2. 分配对齐内存
        void* temp_ptr = nullptr;
        if (posix_memalign(&temp_ptr, page_size, physical_bytes_size) != 0) {
            throw std::runtime_error("MetalAlignedBuffer: Failed to allocate aligned memory");
        }
        data_ptr = static_cast<T*>(temp_ptr);
        
        // 3. 全量清零：Metal 对齐缓冲区建议清理整块物理内存
        std::memset(data_ptr, 0, physical_bytes_size);
        
        // 此处可以预留：如果 engine 不为空，可以在此处调用 MetalEngine 的方法
        // 将 data_ptr 注册到 GPU 资源池中。
    }

    // 析构函数：安全释放
    ~MetalAlignedBuffer() {
        if (data_ptr) {
            std::free(data_ptr);
            data_ptr = nullptr;
        }
    }

    // 禁用拷贝构造和赋值操作，防止内存二次释放
    MetalAlignedBuffer(const MetalAlignedBuffer&) = delete;
    MetalAlignedBuffer& operator=(const MetalAlignedBuffer&) = delete;

    // 获取原始指针（CPU 端访问）
    T* get() const { return data_ptr; }
    
    // 获取对齐后的物理字节数（GPU 绑定时使用）
    size_t physical_bytes() const { return physical_bytes_size; }
    
    // 获取逻辑元素数量
    size_t count() const { return logic_count; }
};

#endif // METAL_ALIGNED_BUFFER_HPP