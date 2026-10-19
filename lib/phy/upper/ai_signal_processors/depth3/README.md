# depth3 — 深度-3 接收机的模型产物

深度-3 接收机把**信道估计（CE）、均衡（EQ）、解映射（DEMOD）作为一个整体**处理，所以它的模型产物
放在这里，而不是放在 `signal_processors/channel_estimator/metal/ai_assets/` 下面 —— 后者是**经典**
信道估计器的产物目录（`helena_*`），二者不是同一条链。

（2026-10-19 由 `signal_processors/channel_estimator/metal/ai_assets/depth3_*` 迁移至此，路径引用同步更新。）

| 目录 | 角色 | 契约要点 |
|---|---|---|
| `depth3_modcond_c32h/` | **默认模型**（2026-10-19 起，`OCUDU_DEPTH3_DEFAULT_MODEL`）| 26 284 参数、c32、16 通道输入、含声明的 SINR 校准 |
| `depth3_modcond_ft/` | 上一代默认（c64，89 420 参数），保留用于回滚与 A/B | 无校准声明 |

默认值在 `apps/units/flexible_o_du/o_du_low/CMakeLists.txt` 的 `OCUDU_DEPTH3_DEFAULT_MODEL` 里；
它是 **CACHE** 变量，切换后已有的 build 目录需要 `cmake -U OCUDU_DEPTH3_DEFAULT_MODEL -S . -B build`
才会重新取值。两个 bundle 都能用 `--expert_phy.pusch_receiver_model=<路径>` 直接指定，无需改默认。
