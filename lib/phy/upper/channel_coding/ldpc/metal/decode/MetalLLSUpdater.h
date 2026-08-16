#ifndef MetalLLSUpdater_h
#define MetalLLSUpdater_h

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /**
    * 初始化 Metal 引擎
    * @param shader_path .metal 文件的绝对路径
    */
   void *init_llr_updater_engine(const char *shader_path);

   /**
    * 预计算 VN 参与度并分配内部缓冲区
    * 内部缓冲区自动 4096 对齐
    */
   void engine_prepare_internal_buffers(void *engine,
                                        const uint32_t *h_raw_ptr,
                                        uint32_t M,
                                        uint32_t n_h_chunks);

   /**
    * 执行 LLS 更新 (双 Kernel 调度)
    */
   void engine_update_lls(void *engine,
                          float alpha,
                          void *llr_ptr,
                          const void *h_matrix,
                          const void *h_pred,
                          void *s_hard,
                          uint32_t M,
                          uint32_t n_h_chunks,
                          void* debug_ptr);

   /**
    * 销毁引擎
    */
   void deinit_llr_updater_engine(void *engine);

#ifdef __cplusplus
}
#endif

#endif /* MetalLLSUpdater_h */