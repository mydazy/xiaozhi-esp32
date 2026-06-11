#include "srmodel_vfs.h"

#include <esp_vfs.h>
#include <esp_log.h>
#include <cstring>
#include <cerrno>
#include <string>
#include <sys/stat.h>

#define TAG "SrmodelVfs"

// model_path.c 有实现但头文件未声明
extern "C" void set_model_base_path(const char* base_path);

namespace {

constexpr int kMaxFd = 4;

struct VfsFile {
    const char* data = nullptr;
    int size = 0;
    int pos = 0;
    bool used = false;
};

srmodel_list_t* s_models = nullptr;
VfsFile s_files[kMaxFd];

// path 形如 "/mn7_cn/mn7_data"（"/srmodel" 前缀已被 VFS 层剥掉）
int VfsOpen(const char* path, int /*flags*/, int /*mode*/) {
    if (s_models == nullptr || path == nullptr || path[0] != '/') {
        errno = ENOENT;
        return -1;
    }
    const char* p = path + 1;
    const char* slash = strchr(p, '/');
    if (slash == nullptr) {
        errno = ENOENT;
        return -1;
    }
    std::string model_name(p, slash - p);
    const char* fname = slash + 1;
    for (int i = 0; i < s_models->num; i++) {
        if (model_name != s_models->model_name[i]) continue;
        srmodel_data_t* md = s_models->model_data[i];
        for (int j = 0; j < md->num; j++) {
            if (strcmp(md->files[j], fname) != 0) continue;
            for (int fd = 0; fd < kMaxFd; fd++) {
                if (!s_files[fd].used) {
                    s_files[fd] = {md->data[j], md->sizes[j], 0, true};
                    return fd;
                }
            }
            errno = ENFILE;
            return -1;
        }
    }
    ESP_LOGW(TAG, "open miss: %s", path);
    errno = ENOENT;
    return -1;
}

ssize_t VfsRead(int fd, void* dst, size_t n) {
    if (fd < 0 || fd >= kMaxFd || !s_files[fd].used) { errno = EBADF; return -1; }
    VfsFile& f = s_files[fd];
    int remain = f.size - f.pos;
    if (remain <= 0) return 0;
    int len = (n < (size_t)remain) ? (int)n : remain;
    memcpy(dst, f.data + f.pos, len);
    f.pos += len;
    return len;
}

off_t VfsLseek(int fd, off_t off, int whence) {
    if (fd < 0 || fd >= kMaxFd || !s_files[fd].used) { errno = EBADF; return -1; }
    VfsFile& f = s_files[fd];
    long npos = (whence == SEEK_SET) ? off
              : (whence == SEEK_CUR) ? f.pos + off
              : f.size + off;
    if (npos < 0 || npos > f.size) { errno = EINVAL; return -1; }
    f.pos = (int)npos;
    return npos;
}

int VfsClose(int fd) {
    if (fd >= 0 && fd < kMaxFd) s_files[fd].used = false;
    return 0;
}

int VfsFstat(int fd, struct stat* st) {
    if (fd < 0 || fd >= kMaxFd || !s_files[fd].used) { errno = EBADF; return -1; }
    memset(st, 0, sizeof(*st));
    st->st_size = s_files[fd].size;
    st->st_mode = S_IFREG;
    return 0;
}

int VfsStat(const char* path, struct stat* st) {
    int fd = VfsOpen(path, 0, 0);
    if (fd < 0) return -1;
    VfsFstat(fd, st);
    VfsClose(fd);
    return 0;
}

}  // namespace

esp_err_t RegisterSrmodelVfs(srmodel_list_t* models) {
    s_models = models;
    esp_vfs_t vfs = {};
    vfs.flags = ESP_VFS_FLAG_DEFAULT;
    vfs.open = VfsOpen;
    vfs.read = VfsRead;
    vfs.lseek = VfsLseek;
    vfs.close = VfsClose;
    vfs.fstat = VfsFstat;
    vfs.stat = VfsStat;
    esp_err_t r = esp_vfs_register("/srmodel", &vfs, nullptr);
    if (r == ESP_OK || r == ESP_ERR_INVALID_STATE) {  // INVALID_STATE = 已注册（资产重载）
        set_model_base_path("/srmodel");
        ESP_LOGI(TAG, "srmodel VFS 就绪 @/srmodel（multinet 经文件接口直读资产 mmap）");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "esp_vfs_register failed: %s", esp_err_to_name(r));
    return r;
}
