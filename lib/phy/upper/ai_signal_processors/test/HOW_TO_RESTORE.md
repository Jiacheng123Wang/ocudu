# 如何还原 `ruler_s2_amc_5000` 的 10 000 个捕获文件

本目录里有两样东西，任选其一即可得到同一批数据：

## 方式一：从分片 tar 还原（**不依赖语料**，推荐）

```bash
cd lib/phy/upper/ai_signal_processors/test
cat ruler_s2_amc_5000.tar.gz.part* > /tmp/ruler.tar.gz
shasum -a 256 /tmp/ruler.tar.gz
# 期望：2431dc6dea9d1e8778b5fa9363343b14c49552d9...
tar -xzf /tmp/ruler.tar.gz -C /tmp
# 得到 /tmp/ruler_s2_amc_5000/corpus/ 下的 10 000 个文件
```

## 方式二：从语料重建（**需要原始捕获**）

```bash
./materialise.sh /path/to/corpus/s2_amc_ping_1102a
```

## 无论哪种方式，都要校验

```bash
shasum -a 256 -c ruler_s2_amc_5000_SHA256SUMS
```

**10 000 条全部 OK 才算对。**

## 为什么分成两片

GitHub 单文件上限 100 MB，而这套文件压缩后 159 MB。两片各 90 MB 与 69 MB，
拼接后与原始 tar.gz 的 SHA256 完全一致（见上面第一段的期望值）。

## 为什么捕获文件本身不入 git

10 000 个 `.bin`/`.txt` 共 355 MB，放进 git 历史后**无法移除**（除非重写历史）。
所以捕获文件以**两个分片 tar** 的形式入库，而**解压后的目录**被 `.gitignore` 排除 ——
这样历史里只有 159 MB 的压缩副本，工作区里可以随时还原。
