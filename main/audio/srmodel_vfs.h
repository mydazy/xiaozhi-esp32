#ifndef SRMODEL_VFS_H
#define SRMODEL_VFS_H

#include <esp_err.h>
#include <model_path.h>

// 把资产分区内存映射的 SR 模型表注册成 /srmodel 虚拟文件系统。
// 背景：libmultinet 的 model_create 只走"拼路径 + fopen"装载（不像 wakenet
// 直接吃内存表），资产 mmap 模型没有文件路径 → mn7 永远装载失败。
// 本 VFS 把 fopen("/srmodel/<model>/<file>") 直接转译为模型表内存指针，零拷贝。
esp_err_t RegisterSrmodelVfs(srmodel_list_t* models);

#endif  // SRMODEL_VFS_H
