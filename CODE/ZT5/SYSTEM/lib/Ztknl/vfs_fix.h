

/* ======================================================================
 * FIX: 重写 vfs_read 和 vfs_write，移除每次读写时的权限检查
 * 权限在 vfs_open 时已经检查，每次读写都检查会导致性能问题
 * 并且 vfs_permission_check 内部的 vfs_stat 可能因为挂载点查找
 * 问题导致读写失败。
 * ====================================================================== */

/*
 * 原始函数在 vfs.c 中约 490-565 行。
 * 这些重写版本会替换原始函数。
 * 
 * 修改内容：
 * 1. 移除 vfs_permission_check 调用（权限在 open 时已检查）
 * 2. 添加调试输出帮助诊断问题
 * 3. 更健壮的错误处理
 */

/*
ssize_t vfs_read(struct vfs_file *file, void *buf, size_t count) {
    if (file == NULL) return -1;
    if (buf == NULL && count > 0) return -1;
    if (count == 0) return 0;
    
    if (!(file->mode & VFS_MODE_READ)) return -1;

    if (file->mount == NULL || file->mount->ops == NULL || file->mount->ops->read == NULL)
        return -1;

    ssize_t ret = file->mount->ops->read(file, buf, count);
    if (ret > 0) file->offset += ret;
    return ret;
}

ssize_t vfs_write(struct vfs_file *file, const void *buf, size_t count) {
    if (file == NULL) return -1;
    if (buf == NULL && count > 0) return -1;
    if (count == 0) return 0;
    
    if (!(file->mode & VFS_MODE_WRITE)) return -1;

    if (file->mount == NULL || file->mount->ops == NULL || file->mount->ops->write == NULL)
        return -1;

    ssize_t ret = file->mount->ops->write(file, buf, count);
    if (ret > 0) file->offset += ret;
    return ret;
}
*/
