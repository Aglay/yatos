/*
 *  Virtual File System
 *
 *  Copyright (C) 2017 ese@ccnt.zju
 *
 *  ---------------------------------------------------
 *  Started at 2017/4/9 by Ray
 *
 *  ---------------------------------------------------
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License.
 */

#include <yatos/list.h>
#include <yatos/ext2.h>
#include <yatos/fs.h>
#include <yatos/mm.h>
#include <yatos/ext2.h>
#include <yatos/task.h>
#include <yatos/sys_call.h>
#include <yatos/task_vmm.h>
#include <yatos/schedule.h>
#include <arch/regs.h>
#include <yatos/errno.h>

static struct kcache * file_cache;
static struct kcache * inode_cache;
static struct kcache * data_buffer_cache;
static struct list_head inode_list;
static struct fs_file * root_dir;

/*
 * Constructor of fs_file.
 * This function will be called by slab_alloc_obj when we alloc a new fs_file struct.
 */
static void fs_constr_file(void *arg)
{
  struct fs_file *file = (struct fs_file*)arg;
  file->count = 1;
}

/*
 * Destructor of fs_file.
 * This function will be called by slab_free_obj when we free a fs_file struct.
 */
static void fs_distr_file(void *arg)
{
  struct fs_file * file = (struct fs_file*)arg;
  if (file->inode && file->inode->action && file->inode->action->close)
    file->inode->action->close(file);
  fs_put_inode(file->inode);
}

/*
 * Constructor of fs_inode.
 * This function will be called by slab_alloc_obj when we alloc a new fs_inode struct.
 */
static void fs_constr_inode(void *arg)
{
  struct fs_inode * inode = (struct fs_inode *)arg;
  inode->count = 1;
  INIT_LIST_HEAD(&(inode->data_buffers));
  INIT_LIST_HEAD(&(inode->list_entry));
}

/*
 * Destructor of fs_inode.
 * This function will be called by slab_free_obj when we free a fs_inode struct.
 */
static void fs_distr_inode(void * arg)
{
  struct fs_inode * inode = (struct fs_inode *)arg;
  struct list_head * cur;
  struct list_head * safe;
  struct fs_data_buffer * data;

  if (inode->action && inode->action->release)
    inode->action->release(inode);
  list_for_each_safe(cur, safe, &(inode->data_buffers)){
    data = container_of(cur, struct fs_data_buffer, list_entry);
    slab_free_obj(data);
  }
  if (!list_empty(&(inode->list_entry)))
      list_del(&(inode->list_entry));
}

/*
 * Destructor of fs_data_buffer.
 * This function will be called by slab_free_obj when we free a fs_data_buffer struct.
 */
static void fs_distr_dbuffer(void *arg)
{
  struct fs_data_buffer * buffer = (struct fs_data_buffer *)arg;

  if (buffer->buffer)
    mm_kfree(buffer->buffer);
}

/*
 * Initate all caches this VFS need.
 * System will go die if any error.
 */
static void fs_init_caches()
{
  file_cache = slab_create_cache(sizeof(struct fs_file), fs_constr_file , fs_distr_file, "fs_file cache");
  assert(file_cache);

  inode_cache = slab_create_cache(sizeof(struct fs_inode), fs_constr_inode, fs_distr_inode, "fs_inode cache");
  assert(inode_cache);

  data_buffer_cache = slab_create_cache(sizeof(struct fs_data_buffer), NULL, fs_distr_dbuffer, "data_buffer cache");
  assert(data_buffer_cache);
}

/*
 * Search inode by "num" from inode hash.
 * Return target inode if successful or return NULL if not found.
 *
 * Note: currently, only inode associated to a real file will be hashed,
 *       that is, always return NULL if we search for a pipe inode or stdio inode.
 */
static struct fs_inode * fs_search_inode(int num)
{
  struct list_head *cur;
  struct fs_inode * ret;

  list_for_each(cur, &inode_list){
    ret = container_of(cur, struct fs_inode, list_entry);
    if (ret->inode_num == num)
      return ret;
  }
  return NULL;
}

/*
 * Add a new inode to inode hash.
 * This function always successful.
 */
static void fs_add_inode(struct fs_inode * inode)
{
  list_add_tail(&(inode->list_entry), &inode_list);
}

/*
 * Get a buffer from inode buffers.
 * This function will create and fill a new buffer if nessary.
 * Return target buffer if successful or return NULL if any error.
 *
 * Note: "block_offset" is the number of fs_data_buffer, that is,
 *        block_offset = 1 means bytes_offset = FS_DATA_BUFFER_SIZE.
 */
struct fs_data_buffer * fs_inode_get_buffer(struct fs_inode * fs_inode, unsigned long block_offset)
{
  struct list_head * cur;
  struct fs_data_buffer * after, * buf;
  char * buffer;

  if (!fs_inode)
    return NULL;
  //check recent buffer;
  if (fs_inode->recent_data && fs_inode->recent_data->block_offset == block_offset)
    return fs_inode->recent_data;

  //search
  list_for_each(cur, &(fs_inode->data_buffers)){
    after = container_of(cur, struct fs_data_buffer, list_entry);
    if (after->block_offset == block_offset){
      fs_inode->recent_data = after;
      return after;
    }
    if (after->block_offset > block_offset)
      break;
  }

  //can not found, add new
  buf = slab_alloc_obj(data_buffer_cache);
  if (!buf)
    return NULL;
  buf->block_offset = block_offset;
  buffer = mm_kmalloc(FS_DATA_BUFFER_SIZE);
  if (!buffer){
    slab_free_obj(buf);
    return NULL;
  }
  buf->buffer = buffer;
  if (ext2_fill_buffer(fs_inode, buf)){
      mm_kfree(buffer);
      slab_free_obj(buf);
      return NULL;
  }
  list_add(&(buf->list_entry), cur->prev);
  fs_inode->recent_data = buf;
  return buf;
}

/*
 * The read function of gerner file.
 * Return read count if successful or return error code if any error.
 */
static int fs_gener_read(struct fs_file* file, char* buffer,unsigned long count)
{
  uint32 read_count = 0;
  uint32 block_offset;
  uint32 buff_offset;
  struct fs_data_buffer * buf;
  uint32 cpy_size;
  struct fs_inode * inode = file->inode;
  uint32 off_set = file->cur_offset;
  uint32 total_size = file->inode->size;

  if (off_set >= total_size)
    return 0;
  if (total_size - off_set < count)
    count = total_size - off_set;

  while (count){
    block_offset = off_set / FS_DATA_BUFFER_SIZE;
    buff_offset = off_set % FS_DATA_BUFFER_SIZE;
    buf = fs_inode_get_buffer(inode, block_offset);
    cpy_size = FS_DATA_BUFFER_SIZE - buff_offset;
    if (cpy_size > count)
      cpy_size = count;

    memcpy(buffer + read_count , buf->buffer + buff_offset, cpy_size);
    off_set += cpy_size;
    count -= cpy_size;
    read_count += cpy_size;
  }
  file->cur_offset += read_count;
  return read_count;
}

/*
 * The write function of gerner file.
 * Return read count if successful or return error code if any error.
 */
static int fs_gener_write(struct fs_file* file,char* buffer,unsigned long count)
{

  struct fs_inode * inode  = file->inode;
  uint32 off_set;
  uint32 write_count = 0;
  uint32 block_offset;
  uint32 buff_offset;
  struct fs_data_buffer * buf;
  uint32 cpy_size;

  if (file->flag & O_APPEND)
    file->cur_offset = file->inode->size;
  off_set = file->cur_offset;
  file->cur_offset += count;

  if (count && file->cur_offset > file->inode->size)
    ext2_truncate(inode, file->cur_offset);

  while (count){
    block_offset = off_set / FS_DATA_BUFFER_SIZE;
    buff_offset = off_set % FS_DATA_BUFFER_SIZE;
    buf = fs_inode_get_buffer(inode, block_offset);
    BUFFER_SET_DIRTY(buf);
    cpy_size = FS_DATA_BUFFER_SIZE - buff_offset;
    if (cpy_size > count)
      cpy_size = count;

    memcpy(buf->buffer + buff_offset , buffer + write_count, cpy_size);
    off_set += cpy_size;
    write_count += cpy_size;
    count -= cpy_size;
  }
  return write_count;
}

/*
 * The sync function of gerner file.
 * Return read count if successful or return error code if any error.
 *
 * Note: this function will auto called once a inode count become zero.
 */
static void fs_gener_sync(struct fs_inode  *inode)
{
  if (inode->links_count > 0)
    ext2_sync_data(inode);
  else {
    // file has been remove
    ext2_truncate(inode, 0);
    ext2_free_inode(inode->inode_num);
  }
}

/*
 * The sync function of gerner file.
 * Return read count if successful or return error code if any error.
 *
 * Note: this function will auto called once a inode being destroyed.
 *       As a Gerner file, inode count become zero does not mean this inode will
 *       be destroyed, it will still saved in inode hash.
 */
static void fs_gener_release(struct fs_inode * inode)
{
  ext2_release_inode((struct ext2_inode*)inode->inode_data);
}

/*
 * The readdir function of Gerner file.
 * Return 0 if successful or return error code if any error.
 *
 * Note: only dir file can do readdir.
 */
static int fs_gener_readdir(struct fs_file * file, struct kdirent *ret)
{
  struct fs_inode * inode = file->inode;
  if (!S_ISDIR(inode->mode))
    return -EINVAL;
  return ext2_readdir(file, ret);
}

/*
 * The seek function of gerner file.
 * This function set the cur_offset of "file".
 * Return cur_offset if successful or return error code if any error.
 */
static off_t fs_gener_seek(struct fs_file* file,off_t offset,int whence)
{
  struct ext2_inode * ext2_inode = file->inode->inode_data;

  switch(whence){
  case SEEK_SET:
    file->cur_offset = offset;
    return file->cur_offset;
  case SEEK_CUR:
    file->cur_offset += offset;
    if (file->cur_offset < 0)
      file->cur_offset = 0;
    return file->cur_offset;
  case SEEK_END:
    file->cur_offset = ext2_inode->i_size + offset;
    return file->cur_offset;
  }
  return -EINVAL;
}

/*
 * The truncate function of gerner file.
 * This function truncate a gerner file as target length.
 * Return 0 if successful or return error code if any error.
 *
 * Note: file can be truncate bigger than current size.
 */
static int fs_gener_truncate(struct fs_file * file, off_t length)
{
  return ext2_truncate(file->inode, length);
}

/*
 * Gerner file inode operations.
 */
static struct fs_inode_oper gerner_inode_oper = {
  .read = fs_gener_read,
  .write = fs_gener_write,
  .seek = fs_gener_seek,
  .sync = fs_gener_sync,
  .release = fs_gener_release,
  .readdir = fs_gener_readdir,
  .ftruncate = fs_gener_truncate,
};

/*
 * Open a file.
 * Return target file struct and *ret=0 if successful or return NULL and *ret=error code if any error.
 */
struct fs_file * fs_open(const char * path, int flag, mode_t mode, int *ret)
{
  //check cur dir
  struct task * task = task_get_cur();
  struct fs_inode * cur_inode;
  struct fs_inode * parent;
  int i = 0 , p = 0, inode_num, new_file = 0;
  struct fs_file * ret_file;
  char name[MAX_FILE_NAME_LEN];
  const char * cur_c;

  //where should we begin
  while (path[i] && (path[i] == ' ' || path[i] == '\t'))
    i++;
  if (!path || !path[i]){
    *ret = -EINVAL;
    return NULL;
  }
  if (path[i] == '/')
    cur_inode = root_dir->inode;
  else
    cur_inode = task->cur_dir->inode;

  // now open
  cur_c = path;
  while (*cur_c){
    p = 0;
    while (*cur_c && *cur_c == '/')
      cur_c++;
    while (*cur_c && *cur_c != '/'){
      name[p++] = *cur_c;
      cur_c++;
    }
    name[p] = '\0';

    if (!p || !strcmp(name, "."))
      continue;

    if (!S_ISDIR(cur_inode->mode)){
      *ret = -ENOTDIR;
      return NULL;
    }

    parent = cur_inode;
    if ((inode_num = ext2_find_file(name, cur_inode)) < 0){
      const char * tmp = cur_c;
      while (*tmp && *tmp == '/')
        tmp++;
      if (*tmp){ //this means what we not find is just a mid dir
        *ret = -ENOENT;
        return NULL;
      }
      //now we creat a new file
      if (flag & O_CREAT){
        if ((inode_num = ext2_create_file(name, cur_inode, mode)) < 0){
          *ret = inode_num;
          return NULL;
        }
      }else{
        *ret = -ENOENT;
        return NULL;
      }
      new_file = 1;
    }
    //look up inode hash.
    cur_inode  = fs_search_inode(inode_num);
    if (!cur_inode){
      cur_inode = slab_alloc_obj(inode_cache);
      if (!cur_inode){
        *ret = -ENOMEM;
        return NULL;
      }
      ext2_fill_inode(cur_inode, inode_num);
      cur_inode->action = &gerner_inode_oper;
      //make count to be zero since we don't realy open it
      cur_inode->count = 0;
      //add to inode cache
      fs_add_inode(cur_inode);
    }
    cur_inode->parent = parent;
  }

  //error of file exist
  if ((flag & O_CREAT) && (flag & O_EXCL) && !new_file){
    *ret = -EEXIST;
    return NULL;
  }
  //truncate ?
  if (flag & O_TRUNC)
    ext2_truncate(cur_inode, 0);

  //ok we have got inode , now  we create fs_file for return
  ret_file = slab_alloc_obj(file_cache);
  if (!ret_file){
    *ret = -ENOMEM;
    return NULL;
  }
  ret_file->inode = cur_inode;
  cur_inode->action = &gerner_inode_oper;
  ret_file->flag = flag;
  //now we should incre file's inode count
  fs_get_inode(cur_inode);
  *ret = 0;
  return ret_file;
}

/*
 * Read data from a file.
 * Return read count if successful or return error code if any error.
 *
 * Note: this function don't know the type of the file.
 */
int fs_read(struct fs_file* file,char* buffer,unsigned long count)
{
  if (file->inode->action->read)
    return file->inode->action->read(file, buffer, count);
  else
    return -EINVAL;
}

/*
 * write data to a file.
 * Return write count if successful or return error code if any error.
 *
 * Note: this function don't know the type of the file.
 */
int fs_write(struct fs_file * file, char * buffer, unsigned long count)
{
  if (file->inode->action->write)
    return file->inode->action->write(file, buffer, count);
  else
    return -EINVAL;
}

/*
 * sync data of a file.
 * Always success.
 *
 * Note: this function don't know the type of the file.
 */
void fs_sync(struct fs_inode * inode)
{
  if (inode->action->sync)
    inode->action->sync(inode);
}

/*
 * Change current offset of a file.
 * Return current offset after doing seek.
 *
 * Note: this function don't know the type of the file.
 */
off_t fs_seek(struct fs_file * file, off_t offset, int whence)
{
  if (file->inode->action->seek)
    return file->inode->action->seek(file, offset, whence);
  else
    return -EINVAL;
  return 0;
}

/*
 * System call of open.
 * This function can only open gerner file now.
 * Return 0 if successful or error code if any error.
 */
static int sys_call_open(struct pt_regs * regs)
{
  const char * path = (const char *)sys_call_arg1(regs);
  int flag = (int)sys_call_arg2(regs);
  mode_t mode = (int)sys_call_arg3(regs);

  struct task * task = task_get_cur();
  struct fs_file * file;
  int fd;
  int ret = 0;
  char * tmp_buffer = (char *)mm_kmalloc(PAGE_SIZE);

  if (!tmp_buffer){
    return -ENOMEM;
  }
  if (task_copy_str_from_user(tmp_buffer, path, MAX_PATH_LEN)){
    ret = -EINVAL;
    goto copy_from_user_error;
  }
  file = fs_open(tmp_buffer, flag, mode, &ret);
  if (!file)
    goto fs_open_error;
  fd = bitmap_alloc(task->fd_map);
  if (fd >= 0){
    mm_kfree(tmp_buffer);
    task->files[fd] = file;
    if (flag & O_CLOEXEC)
      bitmap_set(task->close_on_exec, fd);
    return fd;
  }
  ret = -ENOMEM;

 fs_open_error:
 copy_from_user_error:
  mm_kfree(tmp_buffer);
  return ret;
}

/*
 * System call of read.
 * Read from different srouce according to the action of inode.
 * Return read count if successful or return error code if any error.
 *
 * Note: this function may block task.
 */
static int sys_call_read(struct pt_regs * regs)
{
  int fd = (int)sys_call_arg1(regs);
  char *buffer = (char *)sys_call_arg2(regs);
  unsigned long size = (unsigned long)sys_call_arg3(regs);
  struct task * task = task_get_cur();
  struct fs_file * file;
  char * tmp_buffer;
  int read_n;

  if (fd < 0 || fd >= MAX_OPEN_FD)
    return -EINVAL;
  if (!buffer)
    return -EINVAL;
  file = task->files[fd];
  if (!file || !file->inode || !file->inode->action->read)
    return -EINVAL;
  if (!size)
    return 0;

  tmp_buffer = mm_kmalloc(size);
  if (!tmp_buffer)
    return -ENOMEM;

  read_n = file->inode->action->read(file, tmp_buffer, size);
  if (read_n > 0){
    if (task_copy_to_user(buffer, tmp_buffer, read_n)){
      mm_kfree(tmp_buffer);
      read_n = -EFAULT;
    }
  }
  mm_kfree(tmp_buffer);
  return read_n;
}

/*
 * System call of write.
 * Write to different target according to the action of inode.
 * Return write count if successful or return error code if any error.
 */
static int sys_call_write(struct pt_regs *regs)
{
  int fd = (int)sys_call_arg1(regs);
  char * buffer = (char *)sys_call_arg2(regs);
  unsigned long size = (unsigned long)sys_call_arg3(regs);
  struct task * task = task_get_cur();
  struct fs_file * file;
  char * tmp_buffer;
  int write_n;

  if (fd < 0 || fd >= MAX_OPEN_FD)
    return -EINVAL;
  if (!buffer)
    return -EINVAL;

  file = task->files[fd];
  if (!file || !file->inode || !file->inode->action->write)
    return -EINVAL;

  tmp_buffer = mm_kmalloc(size);
  if (!tmp_buffer)
    return -ENOMEM;

  if (task_copy_from_user(tmp_buffer, buffer, size)){
    mm_kfree(tmp_buffer);
    return -EINVAL;
  }

  write_n = file->inode->action->write(file, tmp_buffer, size);
  mm_kfree(tmp_buffer);
  return write_n;
}

/*
 * System call of seek.
 * Change current offset of file.
 * Return current offset after doing seek or return error code if any error.
 */
static int sys_call_seek(struct pt_regs * regs)
{
  int fd = (int)sys_call_arg1(regs);
  off_t offset = (off_t)sys_call_arg2(regs);
  int whence = (int)sys_call_arg3(regs);
  struct task * task = task_get_cur();
  struct fs_file * file;

  if (fd < 0 || fd >= MAX_OPEN_FD)
    return -EINVAL;
  file = task->files[fd];
  if (!file || !file->inode || !file->inode->action->seek)
    return -EINVAL;
  return file->inode->action->seek(file, offset, whence);
}

/*
 * System call of sync.
 * Write data and inode to disk.
 * Return 0 if successful or return error code if any error.
 */
static int sys_call_sync(struct pt_regs *regs)
{
  int fd = (int)sys_call_arg1(regs);
  struct task * task = task_get_cur();
  struct fs_file * file;

  if (fd < 0 || fd >= MAX_OPEN_FD)
    return -EINVAL;
  file = task->files[fd];
  if (!file || !file->inode || !file->inode->action->sync)
    return -EINVAL;
  file->inode->action->sync(file->inode);
  return 0;
}

/*
 * Close a opened file by fd.
 * Return 0 if successful or return error code if any error.
 */
static int fs_do_close(int fd)
{
  struct task * task = task_get_cur();
  struct fs_file * file = task->files[fd];
  if (!file)
    return -EINVAL;
  fs_put_file(file);
  task->files[fd] = NULL;
  bitmap_free(task->fd_map, fd);
  bitmap_free(task->close_on_exec,fd);
  return 0;
}

/*
 * System call of close.
 * Close a opened file.
 * Return 0 if successful or return error code if any error.
 */
static int sys_call_close(struct pt_regs * regs)
{
  int fd = (int)sys_call_arg1(regs);
  if (fd < 0 || fd >= MAX_OPEN_FD)
    return -EINVAL;
  return fs_do_close(fd);
}

/*
 * System call of ioctl.
 * Control a opened file.
 * Return 0 if successful or return error code if any error.
 */
static int sys_call_ioctl(struct pt_regs * regs)
{
  int fd  = (int)sys_call_arg1(regs);
  int requst = (int)sys_call_arg2(regs);
  unsigned long arg = (unsigned long)sys_call_arg3(regs);
  struct task * task = task_get_cur();
  struct fs_file * file;

  if (fd < 0 || fd >= MAX_OPEN_FD)
    return -EINVAL;

  file = task->files[fd];
  if (!file || !file->inode || !file->inode->action->ioctl)
    return -EINVAL;

  return file->inode->action->ioctl(file, requst, arg);
}

/*
 * System call of readdir.
 * Read a dir entry of file, the file must be a dir.
 * Return 0 and fill sys_call_arg2 if successful or return error code if any error.
 *
 * Note: this function may change current offset of file.
 */
static int sys_call_readdir(struct pt_regs * regs)
{
  int fd = (int)sys_call_arg1(regs);
  struct kdirent * ret_buffer = (struct kdirent *)sys_call_arg2(regs);
  struct task * task = task_get_cur();
  struct fs_file * file;
  struct kdirent tmp_buffer;
  int ret;

  if (fd < 0 || fd >= MAX_OPEN_FD)
    return -EINVAL;
  file = task->files[fd];
  if (!file || !file->inode || !file->inode->action->readdir)
    return -EINVAL;

  if (!(ret = file->inode->action->readdir(file, &tmp_buffer))){
    if (task_copy_to_user(ret_buffer, &tmp_buffer, sizeof(struct kdirent)))
      return -EINVAL;
  }
  return ret;
}

/*
 * System call of make dir;
 * Make a new dir by path.
 * Return 0 if successful or return error code if any error.
 */
static int sys_call_mkdir(struct pt_regs * regs)
{
  const char * path = (const char *)sys_call_arg1(regs);
  mode_t mode = (mode_t)sys_call_arg2(regs);
  char * p_path = (char*)mm_kmalloc(MAX_PATH_LEN + 1);
  int ret = -1;

  if (task_copy_str_from_user(p_path, path, MAX_PATH_LEN)){
    ret = -EINVAL;
    goto copy_error;
  }
  ret = ext2_mkdir(p_path, mode);
 copy_error:
  mm_kfree(p_path);
  return ret;
}

/*
 * System call of unlink.
 * Decrease a link count of a file, if the count is zero, this file will be removed.
 * Return 0 if successful or return error code if any error.
 *
 * Note: dir is not allowed to made link, so unlink is also not allowed to a dir.
 */
static int sys_call_unlink(struct pt_regs * regs)
{
  const char * path = (const char *)sys_call_arg1(regs);
  char * k_path = (char *)mm_kmalloc(MAX_PATH_LEN + 1);
  int ret = -1;

  if (task_copy_str_from_user(k_path, path, MAX_PATH_LEN + 1)){
    ret = -EINVAL;
    goto cp_path_error;
  }
  ret = ext2_unlink(k_path);
 cp_path_error:
  mm_kfree(k_path);
  return ret;
}

/*
 * System call of remove dir.
 * Remove a dir.
 * Return 0 if successful or return error code if any error.
 *
 * Note: target dir must be empty.
 */
static int sys_call_rmdir(struct pt_regs * regs)
{
  const char * path = (const char *)sys_call_arg1(regs);
  int ret = -1;
  char * k_path = (char*)mm_kmalloc(MAX_PATH_LEN + 1);
  if (task_copy_str_from_user(k_path, path, MAX_PATH_LEN + 1)){
    mm_kfree(k_path);
    return -EINVAL;
  }
  ret = ext2_rmdir(k_path);
  mm_kfree(k_path);
  return ret;
}

/*
 * System call of link.
 * Make a hard link to a file.
 * Return 0 if successful or return error code if any error.
 *
 * Note: dir is not allowed to made a link.
 */
static int sys_call_link(struct pt_regs * regs)
{
  const char * oldpath = (const char *)sys_call_arg1(regs);
  const char * newpath = (const char *)sys_call_arg2(regs);
  char * k_oldpath = (char *)mm_kmalloc(MAX_PATH_LEN + 1);
  char * k_newpath = (char *)mm_kmalloc(MAX_PATH_LEN + 1);
  int ret = -1;

  if (task_copy_str_from_user(k_oldpath , oldpath, MAX_PATH_LEN + 1)
      || task_copy_str_from_user(k_newpath, newpath, MAX_PATH_LEN + 1)){
    ret = -EINVAL;
    goto cp_path_error;
  }
  ret = ext2_link(oldpath, newpath);
 cp_path_error:
  mm_kfree(k_oldpath);
  mm_kfree(k_newpath);
  return ret;
}

/*
 * System call of ftruncate.
 * Truncate a file size to target length.
 * Return 0 if successful or return error code if any error.
 *
 * Note: new length can bigger than current file size,
 *       this will make a hole in file and the data in a hole seem to be all zero.
 */
static int sys_call_ftruncate(struct pt_regs * regs)
{
  int fd = (int)sys_call_arg1(regs);
  off_t length = (off_t)sys_call_arg2(regs);
  struct fs_file * file;

  if (fd < 0 || fd >= MAX_OPEN_FD)
    return -EINVAL;

  file = task_get_cur()->files[fd];
  if (!file || !file->inode || !file->inode->action->ftruncate)
    return -EINVAL;
  return file->inode->action->ftruncate(file, length);
}

/*
 * System call of sync, this is different with the system call of fsync.
 * Write all inode ,data buffer and inode bitmap and block bitmap to disk.
 * Return 0 if successful or return error code if any error.
 */
static int sys_call_fssync(struct pt_regs *regs)
{
  struct list_head * cur;
  struct fs_inode * inode;

  list_for_each(cur, &inode_list){
    inode = container_of(cur, struct fs_inode, list_entry);
    fs_sync(inode);
  }
  ext2_sync_system();
  return 0;
}

/*
 * System call of fstat.
 * Get the stat of a inode, such as mode, file size, link count...
 * Return 0 and fill sys_call_arg2 if successful or return error code if any error.
 *
 * Note: this function will not return all the attribute of a inode now.
 */
static int sys_call_fstat(struct pt_regs * regs)
{
  int fd = (int)sys_call_arg1(regs);
  struct kstat * stat = (struct kstat*)sys_call_arg2(regs);
  struct fs_file * file;
  struct kstat kstat;

  if (fd < 0 || fd >= MAX_OPEN_FD)
    return -EINVAL;
  file = task_get_cur()->files[fd];
  if (!file)
    return -EINVAL;
  kstat.inode_num = file->inode->inode_num;
  kstat.links_count = file->inode->links_count;
  kstat.mode = file->inode->mode;
  kstat.size = file->inode->size;
  if (task_copy_to_user(stat, &kstat, sizeof(kstat)))
    return -EINVAL;
  return 0;
}

/*
 * System call of fcntl.
 * Control a opened fd, such as get open flag....
 * Return a vaule >= 0 according to sys_call_arg2 if successful or return error code if any eror.
 */
static int sys_call_fcntl(struct pt_regs * regs)
{
  int fd = (int)sys_call_arg1(regs);
  int cmd = (int)sys_call_arg2(regs);
  int flag = (int)sys_call_arg3(regs);
  struct task * task = task_get_cur();
  int newfd;
  if (fd < 0 || fd >= MAX_OPEN_FD || !task->files[fd])
    return -EINVAL;
  switch(cmd){
  case F_GETFD:
    return bitmap_check(task->close_on_exec, fd);
  case F_SETFD:
    if (flag & FD_CLOEXEC)
      bitmap_set(task->close_on_exec, fd);
    else
      bitmap_free(task->close_on_exec, fd);
    return 0;
  case F_GETFL:
    return task->files[fd]->flag;
  case F_SETFL:
    task->files[fd]->flag = flag;
  case F_DUPFD:
    //dup fd
    newfd = bitmap_alloc(task->fd_map);
    if (newfd < 0)
      return -ENFILE;
    task->files[newfd] = task->files[fd];
    fs_get_file(task->files[fd]);
    return newfd;
  }
  return -EINVAL;
}

/*
 * System call of dup3.
 * Duplicate a fd, if sys_call_arg3=1, new fd will be set CLOC_ON_EXCl.
 * Return 0 if successful or return error code if any error.
 */
static int sys_call_dup3(struct pt_regs * regs)
{
  int oldfd = (int)sys_call_arg1(regs);
  int newfd = (int)sys_call_arg2(regs);
  int flag = (int)sys_call_arg3(regs);
  struct task * task = task_get_cur();
  //check errors
  if (oldfd < 0 || oldfd >= MAX_OPEN_FD)
    return -EINVAL;
  if (newfd < 0 || newfd >= MAX_OPEN_FD)
    return -EINVAL;
  if (!task->files[oldfd])
    return -EINVAL;

  if (task->files[newfd])
    fs_do_close(newfd);

  //copy fd
  bitmap_set(task->fd_map, newfd);
  if (flag)
    bitmap_set(task->close_on_exec, newfd);
  task->files[newfd] = task->files[oldfd];
  fs_get_file(task->files[oldfd]);
  return 0;
}

/*
 * Initate virtual file system.
 * System will go die if any error.
 */
void fs_init()
{

  fs_init_caches();
  INIT_LIST_HEAD(&(inode_list));
  ext2_init();
  root_dir = slab_alloc_obj(file_cache);
  root_dir->inode = slab_alloc_obj(inode_cache);
  root_dir->inode->action = &gerner_inode_oper;
  root_dir->inode->action = &gerner_inode_oper;
  ext2_init_root(root_dir->inode);
  root_dir->inode->count = 0;
  fs_add_inode(root_dir->inode);

  sys_call_regist(SYS_CALL_OPEN, sys_call_open);
  sys_call_regist(SYS_CALL_READ, sys_call_read);
  sys_call_regist(SYS_CALL_WRITE, sys_call_write);
  sys_call_regist(SYS_CALL_SEEK, sys_call_seek);
  sys_call_regist(SYS_CALL_SYNC, sys_call_sync);
  sys_call_regist(SYS_CALL_CLOSE, sys_call_close);
  sys_call_regist(SYS_CALL_IOCTL, sys_call_ioctl);
  sys_call_regist(SYS_CALL_READDIR, sys_call_readdir);
  sys_call_regist(SYS_CALL_MKDIR, sys_call_mkdir);
  sys_call_regist(SYS_CALL_LINK, sys_call_link);
  sys_call_regist(SYS_CALL_UNLINK, sys_call_unlink);
  sys_call_regist(SYS_CALL_RMDIR, sys_call_rmdir);
  sys_call_regist(SYS_CALL_FTRUNCATE, sys_call_ftruncate);
  sys_call_regist(SYS_CALL_FSSYNC, sys_call_fssync);
  sys_call_regist(SYS_CALL_FSTAT, sys_call_fstat);
  sys_call_regist(SYS_CALL_DUP3, sys_call_dup3);
  sys_call_regist(SYS_CALL_FCNTL, sys_call_fcntl);
}

/*
 * Get new file or new inode.
 * The count of new object will be 1.
 */
struct fs_file * fs_new_file()
{
  return slab_alloc_obj(file_cache);
}
struct fs_inode * fs_new_inode()
{
  return slab_alloc_obj(inode_cache);
}
