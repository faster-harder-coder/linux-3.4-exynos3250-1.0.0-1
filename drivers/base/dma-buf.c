/*
 * Framework for buffer objects that can be shared across devices/subsystems.
 *
 * Copyright(C) 2011 Linaro Limited. All rights reserved.
 * Author: Sumit Semwal <sumit.semwal@ti.com>
 *
 * Many thanks to linaro-mm-sig list, and specially
 * Arnd Bergmann <arnd@arndb.de>, Rob Clark <rob@ti.com> and
 * Daniel Vetter <daniel@ffwll.ch> for their support in creation and
 * refining of this idea.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/dma-buf.h>
#include <linux/anon_inodes.h>
#include <linux/export.h>
#include <linux/poll.h>
#include <linux/dmabuf-sync.h>

static int dma_buf_release(struct inode *inode, struct file *file)
{
	struct dma_buf *dmabuf;

	if (!is_dma_buf_file(file))
		return -EINVAL;

	dmabuf = file->private_data;

	BUG_ON(dmabuf->vmapping_counter);

	dmabuf->ops->release(dmabuf);

	dmabuf_sync_reservation_fini(dmabuf);
	kfree(dmabuf);
	return 0;
}

static int dma_buf_mmap_internal(struct file *file, struct vm_area_struct *vma)
{
	struct dma_buf *dmabuf;

	if (!is_dma_buf_file(file))
		return -EINVAL;

	dmabuf = file->private_data;

	/* check for overflowing the buffer's size */
	if (vma->vm_pgoff + ((vma->vm_end - vma->vm_start) >> PAGE_SHIFT) >
	    dmabuf->size >> PAGE_SHIFT)
		return -EINVAL;

	return dmabuf->ops->mmap(dmabuf, vma);
}

static int dma_buf_get_info(struct dma_buf *dmabuf, struct dma_buf_info *info,
				struct file *filp)
{
	info->fence_supported = 0;
	info->size = dmabuf->size;

	return 0;
}

static int dma_buf_get_fence(struct dma_buf *dmabuf, struct dma_buf_fence *df,
				struct file *filp)
{
	struct dmabuf_sync *sync;
	int ret;

	if (WARN_ON(df->ctx))
		return -EBUSY;

	/* Requested by user only in case of 3D GPU. */
	sync = dmabuf_sync_init("3D", NULL, NULL);
	if (IS_ERR(sync)) {
		WARN_ON(1);
		goto err_free;
	}

	ret = dmabuf_sync_get(sync, dmabuf, df->type);
	if (ret < 0) {
		WARN_ON(1);
		dmabuf_sync_fini(sync);
		goto err_free;
	}

	ret = dmabuf_sync_lock(sync);
	if (ret < 0) {
		WARN_ON(1);
		dmabuf_sync_put(sync, dmabuf);
		dmabuf_sync_fini(sync);
		goto err_free;
	}

	df->ctx = (unsigned long)sync;

	return 0;

err_free:
	df->ctx = 0;
	return ret;
}

static int dma_buf_put_fence(struct dma_buf *dmabuf, struct dma_buf_fence *df,
				struct file *filp)
{
	struct dmabuf_sync *sync;
	int ret;

	if (WARN_ON(!df->ctx))
		return -EFAULT;

	sync = (struct dmabuf_sync *)df->ctx;

	ret = dmabuf_sync_unlock(sync);
	if (WARN_ON(ret)) {
		/* TODO */
		return ret;
	}

	dmabuf_sync_put(sync, dmabuf);
	dmabuf_sync_fini(sync);

	df->ctx = 0;

	return 0;
}

static long dma_buf_ioctl(struct file *filp, unsigned int cmd,
				unsigned long arg)
{
	struct dma_buf *dmabuf;
	struct dma_buf_info info;
	struct dma_buf_fence df;
	int ret = 0;

	if (!is_dma_buf_file(filp))
		return -EINVAL;

	dmabuf = filp->private_data;
	if (!dmabuf)
		return -EFAULT;

	switch (cmd) {
	case DMABUF_IOCTL_GET_INFO:
		if (copy_from_user(&info, (struct dma_buf_info *)arg,
					sizeof(info))) {
			ret = -EFAULT;
			break;
		}

		ret = dma_buf_get_info(dmabuf, &info, filp);
		if (ret < 0)
			break;

		if (copy_to_user((struct dma_buf_info *)arg,
					&info, sizeof(info))) {
			ret = -EFAULT;
			break;
		}

		break;

	case DMABUF_IOCTL_GET_FENCE:
		if (!dmabuf->sync) {
			ret = -EPERM;
			break;
		}

		if (copy_from_user(&df, (struct dma_buf_fence *)arg,
					sizeof(df))) {
			ret = -EFAULT;
			break;
		}

		ret = dma_buf_get_fence(dmabuf, &df, filp);
		if (ret < 0)
			break;

		if (copy_to_user((struct dma_buf_fence *)arg,
					&df, sizeof(df))) {
			ret = -EFAULT;
			break;
		}

		break;
	case DMABUF_IOCTL_PUT_FENCE:
		if (!dmabuf->sync) {
			ret = -EPERM;
			break;
		}

		if (copy_from_user(&df, (struct dma_buf_fence *)arg,
					sizeof(df))) {
			ret = -EFAULT;
			break;
		}

		ret = dma_buf_put_fence(dmabuf, &df, filp);
		if (ret < 0)
			break;

		if (copy_to_user((struct dma_buf_fence *)arg,
					&df, sizeof(df))) {
			ret = -EFAULT;
			break;
		}

		break;
	default:
		return -EINVAL;
	}

	return ret;
}

static unsigned int dma_buf_poll(struct file *filp,
					struct poll_table_struct *poll)
{
	struct dma_buf *dmabuf;
	struct dmabuf_sync_reservation *robj;
	int ret = 0;

	if (!is_dma_buf_file(filp))
		return POLLERR;

	dmabuf = filp->private_data;
	if (!dmabuf || !dmabuf->sync)
		return POLLERR;

	robj = dmabuf->sync;

	mutex_lock(&robj->lock);

	robj->polled = true;

	/*
	 * CPU or DMA access to this buffer has been completed, and
	 * the blocked task has been waked up. Return poll event
	 * so that the task can get out of select().
	 */
	if (robj->poll_event) {
		robj->poll_event = false;
		mutex_unlock(&robj->lock);
		return POLLIN | POLLOUT;
	}

	/*
	 * There is no anyone accessing this buffer so just return POLLERR.
	 */
	if (!robj->locked) {
		mutex_unlock(&robj->lock);
		return POLLERR;
	}

	poll_wait(filp, &robj->poll_wait, poll);

	mutex_unlock(&robj->lock);

	return ret;
}

static int dma_buf_lock(struct file *file, int cmd, struct file_lock *fl)
{
	struct dma_buf *dmabuf;
	unsigned int type;
	bool wait = false;

	if (!is_dma_buf_file(file))
		return -EINVAL;

	dmabuf = file->private_data;

	if ((fl->fl_type & F_UNLCK) == F_UNLCK) {
		dmabuf_sync_single_unlock(dmabuf);
		return 0;
	}

	/* convert flock type to dmabuf sync type. */
	if ((fl->fl_type & F_WRLCK) == F_WRLCK)
		type = DMA_BUF_ACCESS_W;
	else if ((fl->fl_type & F_RDLCK) == F_RDLCK)
		type = DMA_BUF_ACCESS_R;
	else
		return -EINVAL;

	if (fl->fl_flags & FL_SLEEP)
		wait = true;

	/* TODO. the locking to certain region should also be considered. */

	return dmabuf_sync_single_lock(dmabuf, type, wait);
}

static const struct file_operations dma_buf_fops = {
	.release	= dma_buf_release,
	.mmap		= dma_buf_mmap_internal,
	.unlocked_ioctl = dma_buf_ioctl,
	.poll		= dma_buf_poll,
	.lock		= dma_buf_lock,
};

/*
 * is_dma_buf_file - Check if struct file* is associated with dma_buf
 */
int is_dma_buf_file(struct file *file)
{
	return file->f_op == &dma_buf_fops;
}

/**
 * dma_buf_export - Creates a new dma_buf, and associates an anon file
 * with this buffer, so it can be exported.
 * Also connect the allocator specific data and ops to the buffer.
 *
 * @priv:	[in]	Attach private data of allocator to this buffer
 * @ops:	[in]	Attach allocator-defined dma buf ops to the new buffer.
 * @size:	[in]	Size of the buffer
 * @flags:	[in]	mode flags for the file.
 *
 * Returns, on success, a newly created dma_buf object, which wraps the
 * supplied private data and operations for dma_buf_ops. On either missing
 * ops, or error in allocating struct dma_buf, will return negative error.
 *
 */
struct dma_buf *dma_buf_export(void *priv, const struct dma_buf_ops *ops,
			       size_t size, int flags)
{
	struct dma_buf *dmabuf;
	struct file *file;
	size_t alloc_size = sizeof(struct dma_buf);

	if (WARN_ON(!priv || !ops
			  || !ops->map_dma_buf
			  || !ops->unmap_dma_buf
			  || !ops->release
			  || !ops->kmap_atomic
			  || !ops->kmap
			  || !ops->mmap)) {
		return ERR_PTR(-EINVAL);
	}

	dmabuf = kzalloc(alloc_size, GFP_KERNEL);
	if (dmabuf == NULL)
		return ERR_PTR(-ENOMEM);

	dmabuf->priv = priv;
	dmabuf->ops = ops;
	dmabuf->size = size;

	file = anon_inode_getfile("dmabuf", &dma_buf_fops, dmabuf, flags);

	dmabuf_sync_reservation_init(dmabuf);
	dmabuf->file = file;

	mutex_init(&dmabuf->lock);
	INIT_LIST_HEAD(&dmabuf->attachments);

	return dmabuf;
}
EXPORT_SYMBOL_GPL(dma_buf_export);


/**
 * dma_buf_fd - returns a file descriptor for the given dma_buf
 * @dmabuf:	[in]	pointer to dma_buf for which fd is required.
 * @flags:      [in]    flags to give to fd
 *
 * On success, returns an associated 'fd'. Else, returns error.
 */
int dma_buf_fd(struct dma_buf *dmabuf, int flags)
{
	int error, fd;

	if (!dmabuf || !dmabuf->file)
		return -EINVAL;

	error = get_unused_fd_flags(flags);
	if (error < 0)
		return error;
	fd = error;

	fd_install(fd, dmabuf->file);

	return fd;
}
EXPORT_SYMBOL_GPL(dma_buf_fd);

/**
 * dma_buf_get - returns the dma_buf structure related to an fd
 * @fd:	[in]	fd associated with the dma_buf to be returned
 *
 * On success, returns the dma_buf structure associated with an fd; uses
 * file's refcounting done by fget to increase refcount. returns ERR_PTR
 * otherwise.
 */
struct dma_buf *dma_buf_get(int fd)
{
	struct file *file;

	file = fget(fd);

	if (!file)
		return ERR_PTR(-EBADF);

	if (!is_dma_buf_file(file)) {
		fput(file);
		return ERR_PTR(-EINVAL);
	}

	return file->private_data;
}
EXPORT_SYMBOL_GPL(dma_buf_get);

/**
 * dma_buf_put - decreases refcount of the buffer
 * @dmabuf:	[in]	buffer to reduce refcount of
 *
 * Uses file's refcounting done implicitly by fput()
 */
void dma_buf_put(struct dma_buf *dmabuf)
{
	if (WARN_ON(!dmabuf || !dmabuf->file))
		return;

	fput(dmabuf->file);
}
EXPORT_SYMBOL_GPL(dma_buf_put);

/**
 * dma_buf_attach - Add the device to dma_buf's attachments list; optionally,
 * calls attach() of dma_buf_ops to allow device-specific attach functionality
 * @dmabuf:	[in]	buffer to attach device to.
 * @dev:	[in]	device to be attached.
 *
 * Returns struct dma_buf_attachment * for this attachment; may return negative
 * error codes.
 *
 */
struct dma_buf_attachment *dma_buf_attach(struct dma_buf *dmabuf,
					  struct device *dev)
{
	struct dma_buf_attachment *attach;
	int ret;

	if (WARN_ON(!dmabuf || !dev))
		return ERR_PTR(-EINVAL);

	attach = kzalloc(sizeof(struct dma_buf_attachment), GFP_KERNEL);
	if (attach == NULL)
		return ERR_PTR(-ENOMEM);

	attach->dev = dev;
	attach->dmabuf = dmabuf;

	mutex_lock(&dmabuf->lock);

	if (dmabuf->ops->attach) {
		ret = dmabuf->ops->attach(dmabuf, dev, attach);
		if (ret)
			goto err_attach;
	}
	list_add(&attach->node, &dmabuf->attachments);

	mutex_unlock(&dmabuf->lock);
	return attach;

err_attach:
	kfree(attach);
	mutex_unlock(&dmabuf->lock);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(dma_buf_attach);

/**
 * dma_buf_detach - Remove the given attachment from dmabuf's attachments list;
 * optionally calls detach() of dma_buf_ops for device-specific detach
 * @dmabuf:	[in]	buffer to detach from.
 * @attach:	[in]	attachment to be detached; is free'd after this call.
 *
 */
void dma_buf_detach(struct dma_buf *dmabuf, struct dma_buf_attachment *attach)
{
	if (WARN_ON(!dmabuf || !attach))
		return;

	mutex_lock(&dmabuf->lock);
	list_del(&attach->node);
	if (dmabuf->ops->detach)
		dmabuf->ops->detach(dmabuf, attach);

	mutex_unlock(&dmabuf->lock);
	kfree(attach);
}
EXPORT_SYMBOL_GPL(dma_buf_detach);

/**
 * dma_buf_map_attachment - Returns the scatterlist table of the attachment;
 * mapped into _device_ address space. Is a wrapper for map_dma_buf() of the
 * dma_buf_ops.
 * @attach:	[in]	attachment whose scatterlist is to be returned
 * @direction:	[in]	direction of DMA transfer
 *
 * Returns sg_table containing the scatterlist to be returned; may return NULL
 * or ERR_PTR.
 *
 */
struct sg_table *dma_buf_map_attachment(struct dma_buf_attachment *attach,
					enum dma_data_direction direction)
{
	struct sg_table *sg_table = ERR_PTR(-EINVAL);

	might_sleep();

	if (WARN_ON(!attach || !attach->dmabuf))
		return ERR_PTR(-EINVAL);

	sg_table = attach->dmabuf->ops->map_dma_buf(attach, direction);

	return sg_table;
}
EXPORT_SYMBOL_GPL(dma_buf_map_attachment);

/**
 * dma_buf_unmap_attachment - unmaps and decreases usecount of the buffer;might
 * deallocate the scatterlist associated. Is a wrapper for unmap_dma_buf() of
 * dma_buf_ops.
 * @attach:	[in]	attachment to unmap buffer from
 * @sg_table:	[in]	scatterlist info of the buffer to unmap
 * @direction:  [in]    direction of DMA transfer
 *
 */
void dma_buf_unmap_attachment(struct dma_buf_attachment *attach,
				struct sg_table *sg_table,
				enum dma_data_direction direction)
{
	if (WARN_ON(!attach || !attach->dmabuf || !sg_table))
		return;

	attach->dmabuf->ops->unmap_dma_buf(attach, sg_table,
						direction);
}
EXPORT_SYMBOL_GPL(dma_buf_unmap_attachment);


/**
 * dma_buf_begin_cpu_access - Must be called before accessing a dma_buf from the
 * cpu in the kernel context. Calls begin_cpu_access to allow exporter-specific
 * preparations. Coherency is only guaranteed in the specified range for the
 * specified access direction.
 * @dma_buf:	[in]	buffer to prepare cpu access for.
 * @start:	[in]	start of range for cpu access.
 * @len:	[in]	length of range for cpu access.
 * @direction:	[in]	length of range for cpu access.
 *
 * Can return negative error values, returns 0 on success.
 */
int dma_buf_begin_cpu_access(struct dma_buf *dmabuf, size_t start, size_t len,
			     enum dma_data_direction direction)
{
	int ret = 0;

	if (WARN_ON(!dmabuf))
		return -EINVAL;

	if (dmabuf->ops->begin_cpu_access)
		ret = dmabuf->ops->begin_cpu_access(dmabuf, start, len, direction);

	return ret;
}
EXPORT_SYMBOL_GPL(dma_buf_begin_cpu_access);

/**
 * dma_buf_end_cpu_access - Must be called after accessing a dma_buf from the
 * cpu in the kernel context. Calls end_cpu_access to allow exporter-specific
 * actions. Coherency is only guaranteed in the specified range for the
 * specified access direction.
 * @dma_buf:	[in]	buffer to complete cpu access for.
 * @start:	[in]	start of range for cpu access.
 * @len:	[in]	length of range for cpu access.
 * @direction:	[in]	length of range for cpu access.
 *
 * This call must always succeed.
 */
void dma_buf_end_cpu_access(struct dma_buf *dmabuf, size_t start, size_t len,
			    enum dma_data_direction direction)
{
	WARN_ON(!dmabuf);

	if (dmabuf->ops->end_cpu_access)
		dmabuf->ops->end_cpu_access(dmabuf, start, len, direction);
}
EXPORT_SYMBOL_GPL(dma_buf_end_cpu_access);

/**
 * dma_buf_kmap_atomic - Map a page of the buffer object into kernel address
 * space. The same restrictions as for kmap_atomic and friensync apply.
 * @dma_buf:	[in]	buffer to map page from.
 * @page_num:	[in]	page in PAGE_SIZE units to map.
 *
 * This call must always succeed, any necessary preparations that might fail
 * need to be done in begin_cpu_access.
 */
void *dma_buf_kmap_atomic(struct dma_buf *dmabuf, unsigned long page_num)
{
	WARN_ON(!dmabuf);

	return dmabuf->ops->kmap_atomic(dmabuf, page_num);
}
EXPORT_SYMBOL_GPL(dma_buf_kmap_atomic);

/**
 * dma_buf_kunmap_atomic - Unmap a page obtained by dma_buf_kmap_atomic.
 * @dma_buf:	[in]	buffer to unmap page from.
 * @page_num:	[in]	page in PAGE_SIZE units to unmap.
 * @vaddr:	[in]	kernel space pointer obtained from dma_buf_kmap_atomic.
 *
 * This call must always succeed.
 */
void dma_buf_kunmap_atomic(struct dma_buf *dmabuf, unsigned long page_num,
			   void *vaddr)
{
	WARN_ON(!dmabuf);

	if (dmabuf->ops->kunmap_atomic)
		dmabuf->ops->kunmap_atomic(dmabuf, page_num, vaddr);
}
EXPORT_SYMBOL_GPL(dma_buf_kunmap_atomic);

/**
 * dma_buf_kmap - Map a page of the buffer object into kernel address space. The
 * same restrictions as for kmap and friensync apply.
 * @dma_buf:	[in]	buffer to map page from.
 * @page_num:	[in]	page in PAGE_SIZE units to map.
 *
 * This call must always succeed, any necessary preparations that might fail
 * need to be done in begin_cpu_access.
 */
void *dma_buf_kmap(struct dma_buf *dmabuf, unsigned long page_num)
{
	WARN_ON(!dmabuf);

	return dmabuf->ops->kmap(dmabuf, page_num);
}
EXPORT_SYMBOL_GPL(dma_buf_kmap);

/**
 * dma_buf_kunmap - Unmap a page obtained by dma_buf_kmap.
 * @dma_buf:	[in]	buffer to unmap page from.
 * @page_num:	[in]	page in PAGE_SIZE units to unmap.
 * @vaddr:	[in]	kernel space pointer obtained from dma_buf_kmap.
 *
 * This call must always succeed.
 */
void dma_buf_kunmap(struct dma_buf *dmabuf, unsigned long page_num,
		    void *vaddr)
{
	WARN_ON(!dmabuf);

	if (dmabuf->ops->kunmap)
		dmabuf->ops->kunmap(dmabuf, page_num, vaddr);
}
EXPORT_SYMBOL_GPL(dma_buf_kunmap);


/**
 * dma_buf_mmap - Setup up a userspace mmap with the given vma
 * @dma_buf:	[in]	buffer that should back the vma
 * @vma:	[in]	vma for the mmap
 * @pgoff:	[in]	offset in pages where this mmap should start within the
 * 			dma-buf buffer.
 *
 * This function adjusts the passed in vma so that it points at the file of the
 * dma_buf operation. It alsog adjusts the starting pgoff and does bounsync
 * checking on the size of the vma. Then it calls the exporters mmap function to
 * set up the mapping.
 *
 * Can return negative error values, returns 0 on success.
 */
int dma_buf_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma,
		 unsigned long pgoff)
{
	if (WARN_ON(!dmabuf || !vma))
		return -EINVAL;

	/* check for offset overflow */
	if (pgoff + ((vma->vm_end - vma->vm_start) >> PAGE_SHIFT) < pgoff)
		return -EOVERFLOW;

	/* check for overflowing the buffer's size */
	if (pgoff + ((vma->vm_end - vma->vm_start) >> PAGE_SHIFT) >
	    dmabuf->size >> PAGE_SHIFT)
		return -EINVAL;

	/* readjust the vma */
	if (vma->vm_file)
		fput(vma->vm_file);

	vma->vm_file = dmabuf->file;
	get_file(vma->vm_file);

	vma->vm_pgoff = pgoff;

	return dmabuf->ops->mmap(dmabuf, vma);
}
EXPORT_SYMBOL_GPL(dma_buf_mmap);

/**
 * dma_buf_vmap - Create virtual mapping for the buffer object into kernel address space. The same restrictions as for vmap and friensync apply.
 * @dma_buf:	[in]	buffer to vmap
 *
 * This call may fail due to lack of virtual mapping address space.
 * These calls are optional in drivers. The intended use for them
 * is for mapping objects linear in kernel space for high use objects.
 * Please attempt to use kmap/kunmap before thinking about these interfaces.
 */
void *dma_buf_vmap(struct dma_buf *dmabuf)
{
	void *ptr;

	if (WARN_ON(!dmabuf))
		return NULL;

	if (!dmabuf->ops->vmap)
		return NULL;

	mutex_lock(&dmabuf->lock);
	if (dmabuf->vmapping_counter) {
		dmabuf->vmapping_counter++;
		BUG_ON(!dmabuf->vmap_ptr);
		ptr = dmabuf->vmap_ptr;
		goto out_unlock;
	}

	BUG_ON(dmabuf->vmap_ptr);

	ptr = dmabuf->ops->vmap(dmabuf);
	if (IS_ERR_OR_NULL(ptr))
		goto out_unlock;

	dmabuf->vmap_ptr = ptr;
	dmabuf->vmapping_counter = 1;

out_unlock:
	mutex_unlock(&dmabuf->lock);
	return ptr;
}
EXPORT_SYMBOL_GPL(dma_buf_vmap);

/**
 * dma_buf_vunmap - Unmap a vmap obtained by dma_buf_vmap.
 * @dma_buf:	[in]	buffer to vmap
 */
void dma_buf_vunmap(struct dma_buf *dmabuf, void *vaddr)
{
	if (WARN_ON(!dmabuf))
		return;

	BUG_ON(!dmabuf->vmap_ptr);
	BUG_ON(dmabuf->vmapping_counter == 0);
	BUG_ON(dmabuf->vmap_ptr != vaddr);

	mutex_lock(&dmabuf->lock);
	if (--dmabuf->vmapping_counter == 0) {
		if (dmabuf->ops->vunmap)
			dmabuf->ops->vunmap(dmabuf, vaddr);
		dmabuf->vmap_ptr = NULL;
	}
	mutex_unlock(&dmabuf->lock);
}
EXPORT_SYMBOL_GPL(dma_buf_vunmap);
