/*
 * be - Provides fall-back methods and helper functions for actual backends
 *
 * Copyright (C) 2015-2017 Javier Gonzáles <javier@cnexlabs.com>
 * Copyright (C) 2015-2017 Matias Bjørling <matias@cnexlabs.com>
 * Copyright (C) 2015-2017 Simon A. F. Lund <slund@cnexlabs.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  - Redistributions of source code must retain the above copyright notice,
 *  this list of conditions and the following disclaimer.
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *  this list of conditions and the following disclaimer in the documentation
 *  and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <errno.h>
#include <stdlib.h>
#include <dirent.h>
#include <liblightnvm.h>
#include <nvm_be.h>
#include <nvm_dev.h>
#include <nvm_utils.h>
#include <nvm_debug.h>

static inline uint64_t _ilog2(uint64_t x)
{
	uint64_t val = 0;

	while (x >>= 1)
		val++;

	return val;
}

static inline void _construct_ppaf_mask(struct nvm_spec_ppaf_nand *ppaf,
					struct nvm_spec_ppaf_nand_mask *mask)
{
	for (int i = 0 ; i < 12; ++i) {
		if ((i % 2)) {
			// i-1 = offset
			// i = width
			mask->a[i/2] = (((uint64_t)1<< ppaf->a[i])-1) << ppaf->a[i-1];
		}
	}
}

struct nvm_dev* nvm_be_nosys_open(const char *NVM_UNUSED(dev_path),
				  int NVM_UNUSED(flags))
{
	NVM_DEBUG("nvm_be_nosys_open");
	errno = ENOSYS;
	return NULL;
}

void nvm_be_nosys_close(struct nvm_dev *NVM_UNUSED(dev))
{
	NVM_DEBUG("nvm_be_nosys_close");
	errno = ENOSYS;
	return;
}

int nvm_be_nosys_user(struct nvm_dev *NVM_UNUSED(dev),
		      struct nvm_cmd *NVM_UNUSED(cmd),
		      struct nvm_ret *NVM_UNUSED(ret))
{
	NVM_DEBUG("nvm_be_nosys_user");
	errno = ENOSYS;
	return -1;
}

int nvm_be_nosys_admin(struct nvm_dev *NVM_UNUSED(dev),
		       struct nvm_cmd *NVM_UNUSED(cmd),
		       struct nvm_ret *NVM_UNUSED(ret))
{
	NVM_DEBUG("nvm_be_nosys_admin");
	errno = ENOSYS;
	return -1;
}

int nvm_be_nosys_vuser(struct nvm_dev *NVM_UNUSED(dev),
		       struct nvm_cmd *NVM_UNUSED(cmd),
		       struct nvm_ret *NVM_UNUSED(ret))
{
	NVM_DEBUG("nvm_be_nosys_vuser");
	errno = ENOSYS;
	return -1;
}

int nvm_be_nosys_vadmin(struct nvm_dev *NVM_UNUSED(dev),
			struct nvm_cmd *NVM_UNUSED(cmd),
			struct nvm_ret *NVM_UNUSED(ret))
{
	NVM_DEBUG("nvm_be_nosys_vadmin");
	errno = ENOSYS;
	return -1;
}

int nvm_be_split_dpath(const char *dev_path, char *nvme_name, int *nsid)
{
	const char prefix[] = "/dev/nvme";
	const size_t prefix_len = strlen(prefix);
	int val;

	if (strlen(dev_path) < prefix_len + 3) {
		errno = EINVAL;
		return -1;
	}

	if (strncmp(prefix, dev_path, prefix_len)) {
		errno = EINVAL;
		return -1;
	}

	val = atoi(&dev_path[strlen(dev_path)-1]);
	if ((val < 1) || (val > 1024)) {
		errno = EINVAL;
		return -1;
	}

	strncpy(nvme_name, dev_path + 5, 5);
	*nsid = val;

	return 0;
}

int nvm_be_sysfs_exists(const char *nvme_name, int nsid)
{
	const int path_buf_len = 0x1000;
	char path_buf[path_buf_len];

	DIR* dir;
	int ret;

	memset(path_buf, 0, sizeof(char) * path_buf_len);
	if (nsid) {
		sprintf(path_buf, "/sys/class/nvme/%s/%sn%d/lightnvm",
			nvme_name, nvme_name, nsid);
	} else {
		sprintf(path_buf, "/sys/class/nvme/%s", nvme_name);
	}

	dir = opendir(path_buf);
	ret = dir != NULL;

	if (dir)
		closedir(dir);

	return ret;
}

int nvm_be_sysfs_to_buf(const char *nvme_name, int nsid, const char *attr,
			char *buf, int buf_len)
{
	const int path_buf_len = 0x1000;
	char path_buf[path_buf_len];
	FILE *fp;
	char c;

	memset(path_buf, 0, sizeof(char) * path_buf_len);
	if (nsid) {
		sprintf(path_buf, "/sys/class/nvme/%s/%sn%d/lightnvm/%s",
			nvme_name, nvme_name, nsid, attr);
	} else {
		sprintf(path_buf, "/sys/class/nvme/%s/%s", nvme_name, attr);
	}

	fp = fopen(path_buf, "rb");
	if (!fp)
		return -1;	// Propagate errno

	memset(buf, 0, sizeof(char) * buf_len);
	for (int i = 0; (((c = getc(fp)) != EOF) && i < buf_len); ++i)
		buf[i] = c;

	fclose(fp);

	return 0;
}

int nvm_be_populate(struct nvm_dev *dev,
	int (*vadmin)(struct nvm_dev *, struct nvm_cmd *, struct nvm_ret *))
{
	struct nvm_geo *geo = &dev->geo;
	struct nvm_cmd cmd = {.cdw={0}};
	struct nvm_spec_identify *idf;
	int err;

	idf = nvm_buf_alloca(4096, sizeof(*idf));
	if (!idf) {
		errno = ENOMEM;
		return -1;
	}
	memset(idf, 0, sizeof(*idf));

	cmd.vadmin.opcode = NVM_S12_OPC_IDF; // Setup command
	cmd.vadmin.addr = (uint64_t)idf;
	cmd.vadmin.data_len = sizeof(*idf);

	err = vadmin(dev, &cmd, NULL);
	if (err) {
		NVM_DEBUG("FAILED: vadmin idf");
		nvm_buf_free(idf);
		return -1; // NOTE: Propagate errno
	}

	switch (idf->s.verid) {
	case NVM_SPEC_VERID_12:
		geo->page_nbytes = idf->s12.grp[0].fpg_sz;
		geo->sector_nbytes = idf->s12.grp[0].csecs;
		geo->meta_nbytes = idf->s12.grp[0].sos;

		geo->nchannels = idf->s12.grp[0].num_ch;
		geo->nluns = idf->s12.grp[0].num_lun;
		geo->nplanes = idf->s12.grp[0].num_pln;
		geo->nblocks = idf->s12.grp[0].num_blk;
		geo->npages = idf->s12.grp[0].num_pg;

		dev->ppaf = idf->s12.ppaf;
		dev->mccap = idf->s12.grp[0].mccap;
		break;

	case NVM_SPEC_VERID_20:
		geo->sector_nbytes = idf->s20.geo.csecs;
		geo->meta_nbytes = idf->s20.geo.sos;
		geo->page_nbytes = idf->s20.wrt.mw_min * geo->sector_nbytes;

		geo->nchannels = idf->s20.geo.num_ch;
		geo->nluns = idf->s20.geo.num_lun;
		geo->nplanes = idf->s20.wrt.mw_opt / idf->s20.wrt.mw_min;
		geo->nblocks = idf->s20.geo.num_cnk;
		geo->npages = ((idf->s20.geo.clba * idf->s20.geo.csecs) / geo->page_nbytes) / geo->nplanes;
		geo->nsectors = geo->page_nbytes / geo->sector_nbytes;

		dev->ppaf = idf->s20.ppaf;
		dev->mccap = idf->s20.mccap;
		break;

	default:
		NVM_DEBUG("Unsupported Version ID(%d)", idf->s.verid);
		errno = ENOSYS;
		nvm_buf_free(idf);
		return -1;
	}

	dev->verid = idf->s.verid;
	_construct_ppaf_mask(&dev->ppaf, &dev->mask);

	nvm_buf_free(idf);

	return 0;
}

/**
 * Derives device quirks based on sysfs serial and device verid
 *
 * WARN: quirk-detection only works when sysfs attributes are available
 * TODO: Re-implement quirk-detection via the device backend
 *
 * @param dev The device to determine quirks and assign quirk_mask
 * @return 0 on success, 1 on error and errno set to indicate the error
 */
static int nvm_be_quirks(struct nvm_dev *dev)
{
	const char serial[] = "CX8800ES";
	const int serial_len = sizeof(serial) - 1;
	const int buf_len = 0x100;
	char buf[buf_len];
	char name[buf_len];
	int nsid = 0;

	memset(name, 0, sizeof(char) * buf_len);
	if (nvm_be_split_dpath(dev->path, name, &nsid)) {
		NVM_DEBUG("FAILED: determining quirks -- split_path");
		return -1;
	}

	if (nvm_be_sysfs_to_buf(name, 0, "serial", buf, buf_len)) {
		NVM_DEBUG("FAILED: determining quirks -- sysfs_to_buf");
		return -1;
	}

	if (strncmp(buf, serial, serial_len)) {
		NVM_DEBUG("INFO: no quirks for serial: %s", serial);
		return 0;
	}

	dev->quirks = NVM_QUIRK_PMODE_ERASE_RUNROLL;
	switch(dev->verid) {
	case NVM_SPEC_VERID_12:
		dev->quirks |= NVM_QUIRK_OOB_2LRG;
		break;

	case NVM_SPEC_VERID_20:
		dev->quirks |= NVM_QUIRK_OOB_READ_1ST4BYTES_NULL;
		break;
	}

	// HOTFIX: for reports of unrealisticly large OOB area
	if ((dev->quirks & NVM_QUIRK_OOB_2LRG) &&
		(dev->geo.meta_nbytes > (dev->geo.sector_nbytes * 0.1))) {
		dev->geo.meta_nbytes = 16; // Naively hope this is right
	}
	
	return 0;
}

int nvm_be_populate_derived(struct nvm_dev *dev)
{
	struct nvm_geo *geo = &dev->geo;

	geo->nsectors = geo->page_nbytes / geo->sector_nbytes;

	/* Derive total number of bytes on device */
	geo->tbytes = geo->nchannels * geo->nluns * \
			geo->nplanes * geo->nblocks * \
			geo->npages * geo->nsectors * \
			geo->sector_nbytes;

	/* Derive the sector-shift-width for LBA mapping */
	dev->ssw = _ilog2(geo->sector_nbytes);

	/* Derive a default plane mode */
	switch(geo->nplanes) {
	case 4:
		dev->pmode = NVM_FLAG_PMODE_QUAD;
		break;
	case 2:
		dev->pmode = NVM_FLAG_PMODE_DUAL;
		break;
	case 1:
		dev->pmode = NVM_FLAG_PMODE_SNGL;
		break;

	default:
		NVM_DEBUG("FAILED: invalid geo->>nplanes: %lu",
			  geo->nplanes);
		errno = EINVAL;
		return -1;
	}

	dev->erase_naddrs_max = NVM_NADDR_MAX;
	dev->write_naddrs_max = NVM_NADDR_MAX;
	dev->read_naddrs_max = NVM_NADDR_MAX;

	dev->meta_mode = NVM_META_MODE_NONE;

	nvm_be_quirks(dev);

	return 0;
}
