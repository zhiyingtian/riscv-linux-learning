参考来源: https://zhuanlan.zhihu.com/p/585395024

# 1. NUMA节点

![img](https://pica.zhimg.com/v2-28492ddbb3700c7c9d2d5c1cb581b27a_1440w.jpg)

## 1.1 内核如何统一组织 NUMA 节点

内核使用了一个大小为 MAX_NUMNODES ，类型为 struct pglist_data 的全局数组 node_data[] 来管理所有的 NUMA 节点。

![img](https://pica.zhimg.com/v2-8bf5f76475860341c329f36aa1fd1956_1440w.jpg)

全局数组 node_data[] 定义在文件 `/arch/arm64(riscv)/include/asm/mmzone.h`中：

node_data[] 数组大小 MAX_NUMNODES 定义在 `/include/linux/numa.h`文件中：

```text
#ifdef CONFIG_NODES_SHIFT
#define NODES_SHIFT     CONFIG_NODES_SHIFT
#else
#define NODES_SHIFT     0
#endif
#define MAX_NUMNODES    (1 << NODES_SHIFT)
```

> UMA 架构下 NODES_SHIFT 为 0 ，所以内核中只用一个 NUMA 节点来管理所有物理内存。

![img](https://pic4.zhimg.com/v2-c2f7d662f6c6178f54e4618e2ecbd549_1440w.jpg)
