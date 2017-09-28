/*
 * be_spdk - Kernel bypassing backend using SPDK
 *
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

#ifndef NVM_BE_SPDK_ENABLED
#include <liblightnvm.h>
#include <nvm_be.h>

struct nvm_be nvm_be_spdk = {
	.id = NVM_BE_SPDK,

	.open = nvm_be_nosys_open,
	.close = nvm_be_nosys_close,

	.user = nvm_be_nosys_user,
	.admin = nvm_be_nosys_admin,

	.vuser = nvm_be_nosys_vuser,
	.vadmin = nvm_be_nosys_vadmin
};
#else
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <spdk/stdinc.h>
#include <spdk/nvme.h>
#include <spdk/env.h>
#include <liblightnvm.h>
#include <omp.h>
#include <nvm_be.h>
#include <nvm_dev.h>
#include <nvm_utils.h>
#include <nvm_debug.h>

#define NVM_BE_SPDK_QPAIR_MAX 64
#define NVM_BE_SPDK_DMA_ALIGNMENT 0x1000

#define NVM_BE_SPDK_QDEPTH_MAX 128

struct nvme_lnvm_cmd {
	/* dword 0 */
	uint16_t opc	: 8;	/* opcode */
	uint16_t flags	: 2;	/* fused operation */
	uint16_t rsvd1	: 6;
	uint16_t cid;		/* command identifier */

	/* dword 1 */
	uint32_t nsid;		/* namespace identifer */

	/* dword 2-3 */
	uint32_t rsvd2;
	uint32_t rsvd3;

	/* dword 4-5 */
	uint64_t mptr;		/* metadata pointer */

	/* dword 6-9 */
	uint64_t prp1;		/* PRP entries */
	uint64_t prp2;
	
	/* dword 10-11 */
	uint64_t ppas;		/* Address list */
	uint16_t nppas;
	uint16_t control;

	uint32_t cdw13;
	uint32_t cdw14;
	uint32_t cdw15;
};
static_assert(sizeof(struct nvme_lnvm_cmd) == 64, "Incorrect size");

struct state {
	struct spdk_nvme_transport_id trid;
	struct spdk_env_opts opts;
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_qpair *qpair;
	struct spdk_nvme_ns *ns;
	uint16_t nsid;

	int attached;
	int admin_outstanding;

	omp_lock_t qpair_lock;
};

static void vio_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	int *completed = cb_arg;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVM_DEBUG("FAILED: spdk_nvme_cpl_is_error");
	}

	*completed = 1;
}

static int vio_execute(struct spdk_nvme_ctrlr *ctrlr,
			struct spdk_nvme_qpair *qpair,
			omp_lock_t *qpair_lock,
			struct spdk_nvme_cmd *nvme_cmd,
			char *payload, size_t payload_len,
			char *meta, size_t NVM_UNUSED(meta_len))
{
	int completed = 0;

	omp_set_lock(qpair_lock);
	if (spdk_nvme_ctrlr_cmd_io_raw_with_md(ctrlr, qpair, nvme_cmd, payload,
					       payload_len, meta, vio_cb,
					       &completed)) {
		NVM_DEBUG("FAILED: spdk_nvme_ctrlr_cmd_io_raw");
		
		omp_unset_lock(qpair_lock);
		return -1;
	}
	omp_unset_lock(qpair_lock);

	while (!completed) {
		omp_set_lock(qpair_lock);
		spdk_nvme_qpair_process_completions(qpair, 0);
		omp_unset_lock(qpair_lock);
	}

	return 0;
}

static inline int nvm_be_spdk_vuser(struct nvm_dev *dev, struct nvm_cmd *cmd,
				    struct nvm_ret *ret)
{
	struct state *state = dev->be_state;

	void *ppas = NULL;
	size_t ppas_len = 0;
	uint64_t ppas_phys = 0;

	void *payload = NULL;
	size_t payload_len = 0;

	void *meta = NULL;
	size_t meta_len = 0;

	struct spdk_nvme_cmd nvme_cmd = { 0 };
	struct nvme_lnvm_cmd *lnvm_cmd = (void*)&nvme_cmd;

	if (ret) {
		ret->status = 0;
		ret->result = 0;
	}

	// Setup NVMe command
	lnvm_cmd->opc = cmd->vuser.opcode;
	lnvm_cmd->nsid = state->nsid;
	lnvm_cmd->control = cmd->vuser.control;

	// Open-Channel SSD specific address list
	lnvm_cmd->nppas = cmd->vuser.nppas;
	if (lnvm_cmd->nppas) {
		ppas_len = (lnvm_cmd->nppas + 1) * sizeof(uint64_t);
		ppas = spdk_dma_zmalloc(ppas_len, NVM_BE_SPDK_DMA_ALIGNMENT,
					&ppas_phys);
		if (!ppas) {
			NVM_DEBUG("FAILED: spdk_dma_zmalloc(ppas)");
			return -1;
		}
		
		memcpy((void*)ppas, (void*)cmd->vuser.ppa_list, ppas_len);
		lnvm_cmd->ppas = ppas_phys;
	} else {
		lnvm_cmd->ppas = cmd->vuser.ppa_list;
	}

	// Allocate and transfer PAYLOAD ( PRP1 + PRP2 )
	if (cmd->vuser.data_len) {
		payload_len = cmd->vuser.data_len;
		payload = spdk_dma_malloc(payload_len,
					  NVM_BE_SPDK_DMA_ALIGNMENT, NULL);
		if (!payload) {
			NVM_DEBUG("FAILED: spdk_dma_malloc(payload)");
			return -1;
		}

		if (cmd->vuser.opcode == NVM_S12_OPC_WRITE)
			memcpy(payload, (void*)cmd->vuser.addr, payload_len);
	}

	// Allocate and transfer META ( MPTR )
	if (cmd->vuser.metadata_len) {
		meta_len = cmd->vuser.metadata_len;
		meta = spdk_dma_malloc(meta_len,
				       NVM_BE_SPDK_DMA_ALIGNMENT, NULL);
		if (!meta) {
			NVM_DEBUG("FAILED: spdk_dma_malloc(meta)");
			spdk_dma_free(payload);
			return -1;
		}

		if (cmd->vuser.opcode == NVM_S12_OPC_WRITE)
			memcpy(meta, (void*)cmd->vuser.metadata, meta_len);
	}

	// Execute the command
	if (vio_execute(state->ctrlr, state->qpair, &state->qpair_lock,
			&nvme_cmd, payload, payload_len, meta, meta_len)) {
		NVM_DEBUG("FAILED: vio_execute");
	}

	// Transfer and de-allocate META ( MPTR )
	if (cmd->vuser.metadata_len) {
		if (cmd->vuser.opcode == NVM_S12_OPC_READ)
			memcpy((void*)cmd->vuser.metadata, meta, meta_len);

		spdk_dma_free(meta);
	}

	// Transfer and de-allocate PAYLOAD ( PRP1 + PRP2 )
	if (cmd->vuser.data_len) {
		if (cmd->vuser.opcode == NVM_S12_OPC_READ)
			memcpy((void*)cmd->vuser.addr, payload, payload_len);

		spdk_dma_free(payload);
	}

	// De-allocate Open-Channel SSD specific address list
	if (cmd->vuser.nppas)
		spdk_dma_free(ppas);

	return 0;
}

static void cpl_admin(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct state *state = cb_arg;

	if (spdk_nvme_cpl_is_error(cpl)) {
		NVM_DEBUG("FAILED: spdk_nvme_cpl_is_error");
	}

	--(state->admin_outstanding);
}

static inline int nvm_be_spdk_vadmin(struct nvm_dev *dev, struct nvm_cmd *cmd,
				     struct nvm_ret *ret)
{
	struct state *state = dev->be_state;

	void *ppas = NULL;
	size_t ppas_len = 0;
	uint64_t ppas_phys = 0;

	void *payload = NULL;
	size_t payload_len = 0;

	struct spdk_nvme_cmd nvme_cmd = { 0 };
	struct nvme_lnvm_cmd *lnvm_cmd = (void*)&nvme_cmd;

	if (ret) {
		ret->status = 0;
		ret->result = 0;
	}

	lnvm_cmd->opc = cmd->vadmin.opcode;
	lnvm_cmd->nsid = state->nsid;

	// Open-Channel SSD specific address list
	lnvm_cmd->nppas = cmd->vadmin.nppas;
	if (lnvm_cmd->nppas) {
		ppas_len = (lnvm_cmd->nppas + 1) * sizeof(uint64_t);
		ppas = spdk_dma_zmalloc(ppas_len,
					NVM_BE_SPDK_DMA_ALIGNMENT,
					&ppas_phys);
		if (!ppas) {
			NVM_DEBUG("FAILED: spdk_dma_malloc(ppas)");
			return -1;
		}
		
		memcpy((void*)ppas, (void*)cmd->vadmin.ppa_list, ppas_len);
		lnvm_cmd->ppas = ppas_phys;
	} else {
		lnvm_cmd->ppas = cmd->vadmin.ppa_list;
	}

	switch(cmd->vadmin.opcode) {
	case NVM_S12_OPC_GET_BBT:
	case NVM_S12_OPC_IDF:
		payload_len = cmd->vadmin.data_len;

		payload = spdk_dma_malloc(payload_len,
					  NVM_BE_SPDK_DMA_ALIGNMENT, NULL);
		if (!payload) {
			NVM_DEBUG("FAILED: spdk_dma_malloc");
			return -1;
		}
		break;

	case NVM_S12_OPC_SET_BBT:
		break;

	default:
		NVM_DEBUG("FAILED: vadmin.opcode: %d", cmd->vadmin.opcode);
		errno = ENOSYS;
		return -1;
	}

	NVM_DEBUG("INFO: spdk_nvme_ctrlr_cmd_admin_raw -- calling...");

	++(state->admin_outstanding);
	if (spdk_nvme_ctrlr_cmd_admin_raw(state->ctrlr, &nvme_cmd, payload,
					  payload_len, cpl_admin, state)) {
		--(state->admin_outstanding);

		NVM_DEBUG("FAILED: spdk_nvme_ctrlr_cmd_admin_raw");
		spdk_dma_free(payload);

		return -1;
	}
	
	NVM_DEBUG("INFO: spdk_nvme_ctrlr_cmd_admin_raw -- called.");

	while (state->admin_outstanding)
		spdk_nvme_ctrlr_process_admin_completions(state->ctrlr);

	if (payload_len) {
		memcpy((void*)cmd->vadmin.addr, payload, payload_len);
		spdk_dma_free(payload);
	}

	if (lnvm_cmd->nppas)
		spdk_dma_free(ppas);

	return 0;
}

/**
 * Attaches only to the device matching the traddr
 */
static bool probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
		     struct spdk_nvme_ctrlr_opts *NVM_UNUSED(opts))
{
	struct state *state = cb_ctx;

	if (spdk_nvme_transport_id_compare(&state->trid, trid)) {
		NVM_DEBUG("trid->traddr: %s != state->trid.traddr: %s",
			  trid->traddr, state->trid.traddr);
		return false;
	}

	return !state->attached;
}

/**
 * Sets up the state{ns, nsid, ctrlr, attached} given via the cb_ctx
 * using the first available name-space.
 */
static void attach_cb(void *cb_ctx,
		      const struct spdk_nvme_transport_id *NVM_UNUSED(trid),
		      struct spdk_nvme_ctrlr *ctrlr,
		      const struct spdk_nvme_ctrlr_opts *NVM_UNUSED(opts))
{
	struct state *state = cb_ctx;
	int num_ns = spdk_nvme_ctrlr_get_num_ns(ctrlr);

	// NOTE: namespace IDs start at 1, not 0.
	for (int nsid = 1; nsid <= num_ns; nsid++) {
		struct spdk_nvme_ns *ns = NULL;
		
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		if (ns == NULL) {
			NVM_DEBUG("skipping invalid nsid: %d", nsid);
			continue;
		}
		if (!spdk_nvme_ns_is_active(ns)) {
			NVM_DEBUG("skipping inactive nsid: %d", nsid);
			continue;
		}
		
		state->ns = ns;
		state->nsid = nsid;
		state->ctrlr = ctrlr;
		state->attached = 1;

		break;
	}
}

void nvm_be_spdk_close(struct nvm_dev *dev)
{
	struct state *state = NULL;

	if (!(dev && dev->be_state))
		return;

	state = dev->be_state;

	if (state->qpair) {
		spdk_nvme_ctrlr_free_io_qpair(state->qpair);
		omp_destroy_lock(&state->qpair_lock);
	}

	if (state->ctrlr)
		spdk_nvme_detach(state->ctrlr);

	free(state);
}

struct nvm_dev *nvm_be_spdk_open(const char *dev_path, int NVM_UNUSED(flags))
{
	int err;
	struct nvm_dev *dev = NULL;
	struct state *state = NULL;

	dev = malloc(sizeof(*dev));
	if (!dev)
		return NULL;	// Propagate `errno` from malloc
	memset(dev, 0, sizeof(*dev));

	state = malloc(sizeof(*state));
	if (!state) {
		nvm_be_spdk_close(dev);
		return NULL;	// Propagate `errno` from malloc
	}
	memset(state, 0, sizeof(*state));

	dev->be_state = state;

	/*
	 * SPDK relies on an abstraction around the local environment named env
	 * that handles memory allocation and PCI device operations.  This
	 * library must be initialized first.
	 */
	spdk_env_opts_init(&(state->opts));
	
	state->opts.name = "liblightnvm";
	state->opts.shm_id = 0;
	state->opts.master_core = 0;

	spdk_env_init(&(state->opts));
	
	/*
	 * Parse the dev_path into transport_id so we can use it to compare to
	 * the probed controller
	 */
	state->trid.trtype = SPDK_NVME_TRANSPORT_PCIE;

	err = spdk_nvme_transport_id_parse(&state->trid, dev_path);
	if (err) {
		errno = -err;

		NVM_DEBUG("FAILED parsing dev_path: %s, err: %d", dev_path, err);
		nvm_be_spdk_close(dev);
		return NULL;
	}

	/*
	 * Start the SPDK NVMe enumeration process. robe_cb will be called for
	 * each NVMe controller found, giving our application a choice on
	 * whether to attach to each controller. attach_cb will then be called
	 * for each controller after the SPDK NVMe driver has completed
	 * initializing the controller we chose to attach.
	 */
	err = spdk_nvme_probe(&state->trid, state, probe_cb, attach_cb, NULL);
	if (err) {
		NVM_DEBUG("FAILED: spdk_nvme_probe(...) -- retrying...");

		err = spdk_nvme_probe(&state->trid, state, probe_cb, attach_cb, NULL);
		if (err) {
			NVM_DEBUG("FAILED: spdk_nvme_probe(...)");
			nvm_be_spdk_close(dev);
			return NULL;
		}
	}

	if (!state->attached) {
		NVM_DEBUG("FAILED: attaching NVMe controller");
		nvm_be_spdk_close(dev);
		return NULL;
	}

	// Setup NVMe IO qpair
	state->qpair = spdk_nvme_ctrlr_alloc_io_qpair(state->ctrlr, NULL, 0);
	if (!state->qpair) {
		NVM_DEBUG("FAILED: allocating qpair");
		nvm_be_spdk_close(dev);
		return NULL;
	}

	// Setup IO qpair lock
	omp_init_lock(&state->qpair_lock);

	err = nvm_be_populate(dev, nvm_be_spdk_vadmin);
	if (err) {
		NVM_DEBUG("FAILED: nvm_be_populate, err(%d)", err);
		nvm_be_spdk_close(dev);
		return NULL;
	}

	err = nvm_be_populate_derived(dev);
	if (err) {
		NVM_DEBUG("FAILED: nvm_be_populate_derived");
		nvm_be_spdk_close(dev);
		return NULL;
	}

	NVM_DEBUG("Good to go!?");

	return dev;
}

struct nvm_be nvm_be_spdk = {
	.id = NVM_BE_SPDK,

	.open = nvm_be_spdk_open,
	.close = nvm_be_spdk_close,

	.user = nvm_be_nosys_user,
	.admin = nvm_be_nosys_admin,

	.vuser = nvm_be_spdk_vuser,
	.vadmin = nvm_be_spdk_vadmin,
};
#endif
