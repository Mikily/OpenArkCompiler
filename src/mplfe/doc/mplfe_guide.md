# FE前端框架使用手册
[TOC]

## 附录A. 并行编程规范
### A.1. 并行过程中不可使用的操作
FE前端框架使用多线程并行, 为了保证高效性, 不可以使用以下的操作:
* 写入```GlobalTable```
* 使用```MemPool```进行对象创建

### A.2. 规范守护框架
FE前端框架提供宏进行并行可行性检测, 具体使用步骤如下
1. 添加头文件```fe_config_parallel.h```
2. 根据<a href="###A.1.">A.1.</a>的规范进行函数并行可行性判断, 如果不可以并行, 在函数入口处添加宏调用```MPLFE_PARALLEL_FORBIDDEN()```

如果在并行过程中调用了宏```MPLFE_PARALLEL_FORBIDDEN()```, 则会引发```CHECK_FATAL```错误, 并打印调用点的行号.
注: ```-np 1```也是并行调用过程, 使用1个```Run```线程加1个```Finish```线程.
