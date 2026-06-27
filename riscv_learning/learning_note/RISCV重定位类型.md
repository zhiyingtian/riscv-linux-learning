# RISCV重定位类型
```C
typedef struct {
    Elf64_Addr    r_offset;  // 重定位位置：需要修正的地址偏移
    Elf64_Xword   r_info;    // 重定位信息：高32位为符号索引，低32位为重定位类型
    Elf64_Sxword  r_addend;  // 附加数：用于计算最终地址的常量偏移
} Elf64_Rela;
```
## R_RISCV_RELATIVE‌
相对重定位类型，用于初始化指向库内部符号的指针。

| 结构体字段 | 在 R_RISCV_RELATIVE 中的含义 | 示例值 |
| --- | --- | --- |
| ‌r_offset‌ | 需要被初始化的‌指针变量‌在内存中的位置（相对于加载基址） | 0x2010 |
| ‌r_info‌ | 包含重定位类型 R_RISCV_RELATIVE (值为 3)，且符号索引部分为 0 | 0x0000000000000003 |
| ‌r_addend‌ | 指针应指向的‌目标地址‌相对于加载基址的固定偏移 | 0x2000 |

当共享库被加载到内存时,动态连接器将对每条 R_RISCV_RELATIVE 条目进行修正,将指针变量的值设置为目标地址的值。

共享库示例代码:
```c
int global_var = 42;
int *ptr_to_global = &global_var;  // 触发 R_RISCV_RELATIVE
```

* **r_offset** 为ptr_to_global指针相对于库的偏移,假如为0x2010
* **r_addend** 指示ptr_to_global指针所指向的目标地址,此处目标地址为global_var的地址,假如为0x3000
当共享库加载到内存时,其共享库的基址也会确定.假如为0x4F000000.
则:
ptr_to_global指针的值为0x4F000000 + 0x2010 = 0x4F000010
global_var的变量地址为0x4F000000 + 0x3000 = 0x4F000330
那么就可以对此指针指向的地址进行修正:
*0x4F000010 = 0x4F000330 // *ptr_to_global = &global_var;
**总结:**
R_RISCV_RELATIVE重定位类型用于初始化指向库内部符号的指针。当共享库被加载到内存时,动态连接器将对每条R_RISCV_RELATIVE条目进行修正,将指针变量的值设置为目标地址的值。计算公式为:
```c
*(base + r_offset) = base + r_addend
```