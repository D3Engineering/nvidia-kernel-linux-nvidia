/*
 * drivers/video/tegra/host/bus_client.c
 *
 * Tegra Graphics Host Client Module
 *
 * Copyright (c) 2010-2012, NVIDIA Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/slab.h>
#include <linux/string.h>
#include <linux/spinlock.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/file.h>
#include <linux/clk.h>
#include <linux/hrtimer.h>
#include <linux/export.h>

#include <trace/events/nvhost.h>

#include <linux/io.h>
#include <linux/string.h>

#include <linux/nvhost.h>
#include <linux/nvhost_ioctl.h>

#include <mach/nvmap.h>
#include <mach/gpufuse.h>
#include <mach/hardware.h>
#include <mach/iomap.h>

#include "debug.h"
#include "bus_client.h"
#include "dev.h"

void nvhost_read_module_regs(struct nvhost_device *ndev,
			u32 offset, int count, u32 *values)
{
	void __iomem *p = ndev->aperture + offset;

	nvhost_module_busy(ndev);
	while (count--) {
		*(values++) = readl(p);
		p += 4;
	}
	rmb();
	nvhost_module_idle(ndev);
}

void nvhost_write_module_regs(struct nvhost_device *ndev,
			u32 offset, int count, const u32 *values)
{
	void __iomem *p = ndev->aperture + offset;

	nvhost_module_busy(ndev);
	while (count--) {
		writel(*(values++), p);
		p += 4;
	}
	wmb();
	nvhost_module_idle(ndev);
}

struct nvhost_channel_userctx {
	struct nvhost_channel *ch;
	struct nvhost_hwctx *hwctx;
	struct nvhost_submit_hdr_ext hdr;
	int num_relocshifts;
	struct nvhost_job *job;
	struct nvmap_client *nvmap;
	u32 timeout;
	u32 priority;
	int clientid;
};

/*
 * Write cmdbuf to ftrace output. Checks if cmdbuf contents should be output
 * and mmaps the cmdbuf contents if required.
 */
static void trace_write_cmdbufs(struct nvhost_job *job)
{
	struct nvmap_handle_ref handle;
	void *mem = NULL;
	int i = 0;

	for (i = 0; i < job->num_gathers; i++) {
		struct nvhost_channel_gather *gather = &job->gathers[i];
		if (nvhost_debug_trace_cmdbuf) {
			handle.handle = nvmap_id_to_handle(gather->mem_id);
			mem = nvmap_mmap(&handle);
			if (IS_ERR_OR_NULL(mem))
				mem = NULL;
		};

		if (mem) {
			u32 i;
			/*
			 * Write in batches of 128 as there seems to be a limit
			 * of how much you can output to ftrace at once.
			 */
			for (i = 0; i < gather->words; i += TRACE_MAX_LENGTH) {
				trace_nvhost_channel_write_cmdbuf_data(
					job->ch->dev->name,
					gather->mem_id,
					min(gather->words - i,
					    TRACE_MAX_LENGTH),
					gather->offset + i * sizeof(u32),
					mem);
			}
			nvmap_munmap(&handle, mem);
		}
	}
}

static int nvhost_channelrelease(struct inode *inode, struct file *filp)
{
	struct nvhost_channel_userctx *priv = filp->private_data;

	trace_nvhost_channel_release(priv->ch->dev->name);

	filp->private_data = NULL;

	nvhost_module_remove_client(priv->ch->dev, priv);
	nvhost_putchannel(priv->ch, priv->hwctx);

	if (priv->hwctx)
		priv->ch->ctxhandler->put(priv->hwctx);

	if (priv->job)
		nvhost_job_put(priv->job);

	nvmap_client_put(priv->nvmap);
	kfree(priv);
	return 0;
}

static int nvhost_channelopen(struct inode *inode, struct file *filp)
{
	struct nvhost_channel_userctx *priv;
	struct nvhost_channel *ch;

	ch = container_of(inode->i_cdev, struct nvhost_channel, cdev);
	ch = nvhost_getchannel(ch);
	if (!ch)
		return -ENOMEM;
	trace_nvhost_channel_open(ch->dev->name);

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		nvhost_putchannel(ch, NULL);
		return -ENOMEM;
	}
	filp->private_data = priv;
	priv->ch = ch;
	nvhost_module_add_client(ch->dev, priv);

	if (ch->ctxhandler && ch->ctxhandler->alloc) {
		priv->hwctx = ch->ctxhandler->alloc(ch->ctxhandler, ch);
		if (!priv->hwctx)
			goto fail;
	}
	priv->priority = NVHOST_PRIORITY_MEDIUM;
	priv->clientid = atomic_add_return(1,
			&nvhost_get_host(ch->dev)->clientid);

	priv->job = nvhost_job_alloc(ch, priv->hwctx, &priv->hdr,
			NULL, priv->priority, priv->clientid);
	if (!priv->job)
		goto fail;

	return 0;
fail:
	nvhost_channelrelease(inode, filp);
	return -ENOMEM;
}

static int set_submit(struct nvhost_channel_userctx *ctx)
{
	struct device *device = &ctx->ch->dev->dev;

	/* submit should have at least 1 cmdbuf */
	if (!ctx->hdr.num_cmdbufs)
		return -EIO;

	if (!ctx->nvmap) {
		dev_err(device, "no nvmap context set\n");
		return -EFAULT;
	}

	ctx->job = nvhost_job_realloc(ctx->job,
			ctx->hwctx,
			&ctx->hdr,
			ctx->nvmap,
			ctx->priority,
			ctx->clientid);
	if (!ctx->job)
		return -ENOMEM;
	ctx->job->timeout = ctx->timeout;

	if (ctx->hdr.submit_version >= NVHOST_SUBMIT_VERSION_V2)
		ctx->num_relocshifts = ctx->hdr.num_relocs;

	return 0;
}

static void reset_submit(struct nvhost_channel_userctx *ctx)
{
	ctx->hdr.num_cmdbufs = 0;
	ctx->hdr.num_relocs = 0;
	ctx->num_relocshifts = 0;
	ctx->hdr.num_waitchks = 0;
}

static ssize_t nvhost_channelwrite(struct file *filp, const char __user *buf,
				size_t count, loff_t *offp)
{
	struct nvhost_channel_userctx *priv = filp->private_data;
	size_t remaining = count;
	int err = 0;
	struct nvhost_job *job = priv->job;
	struct nvhost_submit_hdr_ext *hdr = &priv->hdr;
	const char *chname = priv->ch->dev->name;

	if (!job)
		return -EIO;

	while (remaining) {
		size_t consumed;
		if (!hdr->num_relocs &&
		    !priv->num_relocshifts &&
		    !hdr->num_cmdbufs &&
		    !hdr->num_waitchks) {
			consumed = sizeof(struct nvhost_submit_hdr);
			if (remaining < consumed)
				break;
			if (copy_from_user(hdr, buf, consumed)) {
				err = -EFAULT;
				break;
			}
			hdr->submit_version = NVHOST_SUBMIT_VERSION_V0;
			err = set_submit(priv);
			if (err)
				break;
			trace_nvhost_channel_write_submit(chname,
			  count, hdr->num_cmdbufs, hdr->num_relocs,
			  hdr->syncpt_id, hdr->syncpt_incrs);
		} else if (hdr->num_cmdbufs) {
			struct nvhost_cmdbuf cmdbuf;
			consumed = sizeof(cmdbuf);
			if (remaining < consumed)
				break;
			if (copy_from_user(&cmdbuf, buf, consumed)) {
				err = -EFAULT;
				break;
			}
			trace_nvhost_channel_write_cmdbuf(chname,
				cmdbuf.mem, cmdbuf.words, cmdbuf.offset);
			nvhost_job_add_gather(job,
				cmdbuf.mem, cmdbuf.words, cmdbuf.offset);
			hdr->num_cmdbufs--;
		} else if (hdr->num_relocs) {
			consumed = sizeof(struct nvhost_reloc);
			if (remaining < consumed)
				break;
			if (copy_from_user(&job->pinarray[job->num_pins],
					buf, consumed)) {
				err = -EFAULT;
				break;
			}
			trace_nvhost_channel_write_reloc(chname);
			job->num_pins++;
			hdr->num_relocs--;
		} else if (hdr->num_waitchks) {
			int numwaitchks =
				(remaining / sizeof(struct nvhost_waitchk));
			if (!numwaitchks)
				break;
			numwaitchks = min_t(int,
				numwaitchks, hdr->num_waitchks);
			consumed = numwaitchks * sizeof(struct nvhost_waitchk);
			if (copy_from_user(&job->waitchk[job->num_waitchk],
					buf, consumed)) {
				err = -EFAULT;
				break;
			}
			trace_nvhost_channel_write_waitchks(
			  chname, numwaitchks,
			  hdr->waitchk_mask);
			job->num_waitchk += numwaitchks;
			hdr->num_waitchks -= numwaitchks;
		} else if (priv->num_relocshifts) {
			int next_shift =
				job->num_pins - priv->num_relocshifts;
			consumed = sizeof(struct nvhost_reloc_shift);
			if (remaining < consumed)
				break;
			if (copy_from_user(
					&job->pinarray[next_shift].reloc_shift,
					buf, consumed)) {
				err = -EFAULT;
				break;
			}
			priv->num_relocshifts--;
		} else {
			err = -EFAULT;
			break;
		}
		remaining -= consumed;
		buf += consumed;
	}

	if (err < 0) {
		dev_err(&priv->ch->dev->dev, "channel write error\n");
		reset_submit(priv);
		return err;
	}

	return count - remaining;
}

static int nvhost_ioctl_channel_flush(
	struct nvhost_channel_userctx *ctx,
	struct nvhost_get_param_args *args,
	int null_kickoff)
{
	struct device *device = &ctx->ch->dev->dev;
	int err;

	trace_nvhost_ioctl_channel_flush(ctx->ch->dev->name);

	if (!ctx->job ||
	    ctx->hdr.num_relocs ||
	    ctx->hdr.num_cmdbufs ||
	    ctx->hdr.num_waitchks) {
		reset_submit(ctx);
		dev_err(device, "channel submit out of sync\n");
		return -EFAULT;
	}

	err = nvhost_job_pin(ctx->job);
	if (err) {
		dev_warn(device, "nvhost_job_pin failed: %d\n", err);
		return err;
	}

	if (nvhost_debug_null_kickoff_pid == current->tgid)
		null_kickoff = 1;
	ctx->job->null_kickoff = null_kickoff;

	if ((nvhost_debug_force_timeout_pid == current->tgid) &&
	    (nvhost_debug_force_timeout_channel == ctx->ch->chid)) {
		ctx->timeout = nvhost_debug_force_timeout_val;
	}

	trace_write_cmdbufs(ctx->job);

	/* context switch if needed, and submit user's gathers to the channel */
	err = nvhost_channel_submit(ctx->job);
	args->value = ctx->job->syncpt_end;
	if (err)
		nvhost_job_unpin(ctx->job);

	return err;
}

static int nvhost_ioctl_channel_read_3d_reg(
	struct nvhost_channel_userctx *ctx,
	struct nvhost_read_3d_reg_args *args)
{
	BUG_ON(!channel_op(ctx->ch).read3dreg);
	return channel_op(ctx->ch).read3dreg(ctx->ch, ctx->hwctx,
			args->offset, &args->value);
}

static long nvhost_channelctl(struct file *filp,
	unsigned int cmd, unsigned long arg)
{
	struct nvhost_channel_userctx *priv = filp->private_data;
	u8 buf[NVHOST_IOCTL_CHANNEL_MAX_ARG_SIZE];
	int err = 0;

	if ((_IOC_TYPE(cmd) != NVHOST_IOCTL_MAGIC) ||
		(_IOC_NR(cmd) == 0) ||
		(_IOC_NR(cmd) > NVHOST_IOCTL_CHANNEL_LAST))
		return -EFAULT;

	BUG_ON(_IOC_SIZE(cmd) > NVHOST_IOCTL_CHANNEL_MAX_ARG_SIZE);

	if (_IOC_DIR(cmd) & _IOC_WRITE) {
		if (copy_from_user(buf, (void __user *)arg, _IOC_SIZE(cmd)))
			return -EFAULT;
	}

	switch (cmd) {
	case NVHOST_IOCTL_CHANNEL_FLUSH:
		err = nvhost_ioctl_channel_flush(priv, (void *)buf, 0);
		break;
	case NVHOST_IOCTL_CHANNEL_NULL_KICKOFF:
		err = nvhost_ioctl_channel_flush(priv, (void *)buf, 1);
		break;
	case NVHOST_IOCTL_CHANNEL_SUBMIT_EXT:
	{
		struct nvhost_submit_hdr_ext *hdr;

		if (priv->hdr.num_relocs ||
		    priv->num_relocshifts ||
		    priv->hdr.num_cmdbufs ||
		    priv->hdr.num_waitchks) {
			reset_submit(priv);
			dev_err(&priv->ch->dev->dev,
				"channel submit out of sync\n");
			err = -EIO;
			break;
		}

		hdr = (struct nvhost_submit_hdr_ext *)buf;
		if (hdr->submit_version > NVHOST_SUBMIT_VERSION_MAX_SUPPORTED) {
			dev_err(&priv->ch->dev->dev,
				"submit version %d > max supported %d\n",
				hdr->submit_version,
				NVHOST_SUBMIT_VERSION_MAX_SUPPORTED);
			err = -EINVAL;
			break;
		}
		memcpy(&priv->hdr, hdr, sizeof(struct nvhost_submit_hdr_ext));
		err = set_submit(priv);
		trace_nvhost_ioctl_channel_submit(priv->ch->dev->name,
			priv->hdr.submit_version,
			priv->hdr.num_cmdbufs, priv->hdr.num_relocs,
			priv->hdr.num_waitchks,
			priv->hdr.syncpt_id, priv->hdr.syncpt_incrs);
		break;
	}
	case NVHOST_IOCTL_CHANNEL_GET_SYNCPOINTS:
		/* host syncpt ID is used by the RM (and never be given out) */
		BUG_ON(priv->ch->dev->syncpts & (1 << NVSYNCPT_GRAPHICS_HOST));
		((struct nvhost_get_param_args *)buf)->value =
			priv->ch->dev->syncpts;
		break;
	case NVHOST_IOCTL_CHANNEL_GET_WAITBASES:
		((struct nvhost_get_param_args *)buf)->value =
			priv->ch->dev->waitbases;
		break;
	case NVHOST_IOCTL_CHANNEL_GET_MODMUTEXES:
		((struct nvhost_get_param_args *)buf)->value =
			priv->ch->dev->modulemutexes;
		break;
	case NVHOST_IOCTL_CHANNEL_SET_NVMAP_FD:
	{
		int fd = (int)((struct nvhost_set_nvmap_fd_args *)buf)->fd;
		struct nvmap_client *new_client = nvmap_client_get_file(fd);

		if (IS_ERR(new_client)) {
			err = PTR_ERR(new_client);
			break;
		}

		if (priv->nvmap)
			nvmap_client_put(priv->nvmap);

		priv->nvmap = new_client;
		break;
	}
	case NVHOST_IOCTL_CHANNEL_READ_3D_REG:
		err = nvhost_ioctl_channel_read_3d_reg(priv, (void *)buf);
		break;
	case NVHOST_IOCTL_CHANNEL_GET_CLK_RATE:
	{
		unsigned long rate;
		struct nvhost_clk_rate_args *arg =
				(struct nvhost_clk_rate_args *)buf;

		err = nvhost_module_get_rate(priv->ch->dev, &rate, 0);
		if (err == 0)
			arg->rate = rate;
		break;
	}
	case NVHOST_IOCTL_CHANNEL_SET_CLK_RATE:
	{
		struct nvhost_clk_rate_args *arg =
				(struct nvhost_clk_rate_args *)buf;
		unsigned long rate = (unsigned long)arg->rate;

		err = nvhost_module_set_rate(priv->ch->dev, priv, rate, 0);
		break;
	}
	case NVHOST_IOCTL_CHANNEL_SET_TIMEOUT:
		priv->timeout =
			(u32)((struct nvhost_set_timeout_args *)buf)->timeout;
		dev_dbg(&priv->ch->dev->dev,
			"%s: setting buffer timeout (%d ms) for userctx 0x%p\n",
			__func__, priv->timeout, priv);
		break;
	case NVHOST_IOCTL_CHANNEL_GET_TIMEDOUT:
		((struct nvhost_get_param_args *)buf)->value =
				priv->hwctx->has_timedout;
		break;
	case NVHOST_IOCTL_CHANNEL_SET_PRIORITY:
		priv->priority =
			(u32)((struct nvhost_set_priority_args *)buf)->priority;
		break;
	default:
		err = -ENOTTY;
		break;
	}

	if ((err == 0) && (_IOC_DIR(cmd) & _IOC_READ))
		err = copy_to_user((void __user *)arg, buf, _IOC_SIZE(cmd));

	return err;
}

static const struct file_operations nvhost_channelops = {
	.owner = THIS_MODULE,
	.release = nvhost_channelrelease,
	.open = nvhost_channelopen,
	.write = nvhost_channelwrite,
	.unlocked_ioctl = nvhost_channelctl
};

int nvhost_client_user_init(struct nvhost_device *dev)
{
	int err, devno;

	struct nvhost_channel *ch = dev->channel;

	cdev_init(&ch->cdev, &nvhost_channelops);
	ch->cdev.owner = THIS_MODULE;

	devno = MKDEV(nvhost_major, nvhost_minor + dev->index);
	err = cdev_add(&ch->cdev, devno, 1);
	if (err < 0) {
		dev_err(&dev->dev,
			"failed to add chan %i cdev\n", dev->index);
		goto fail;
	}
	ch->node = device_create(nvhost_get_host(dev)->nvhost_class, NULL, devno, NULL,
			IFACE_NAME "-%s", dev->name);
	if (IS_ERR(ch->node)) {
		err = PTR_ERR(ch->node);
		dev_err(&dev->dev,
			"failed to create %s channel device\n", dev->name);
		goto fail;
	}

	return 0;
fail:
	return err;
}

int nvhost_client_device_init(struct nvhost_device *dev)
{
	int err;
	struct nvhost_master *nvhost_master = nvhost_get_host(dev);
	struct nvhost_channel *ch = &nvhost_master->channels[dev->index];

	/* store the pointer to this device for channel */
	ch->dev = dev;

	err = nvhost_channel_init(ch, nvhost_master, dev->index);
	if (err)
		goto fail;

	err = nvhost_client_user_init(dev);
	if (err)
		goto fail;

	err = nvhost_module_init(dev);
	if (err)
		goto fail;

	dev_info(&dev->dev, "initialized\n");

	return 0;

fail:
	/* Add clean-up */
	return err;
}

int nvhost_client_device_suspend(struct nvhost_device *dev)
{
	int ret = 0;

	dev_info(&dev->dev, "suspending\n");

	ret = nvhost_channel_suspend(dev->channel);
	if (ret)
		return ret;

	dev_info(&dev->dev, "suspend status: %d\n", ret);

	return ret;
}
