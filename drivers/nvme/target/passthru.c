// SPDX-License-Identifier: GPL-2.0
/*
 * NVMe Over Fabrics Target Passthrough command implementation.
 *
 * Copyright (c) 2017-2018 Western Digital Corporation or its
 * affiliates.
 * Copyright (c) 2019-2020, Eideticom Inc.
 *
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/module.h>

#include "../host/nvme.h"
#include "nvmet.h"

MODULE_IMPORT_NS("NVME_TARGET_PASSTHRU");

/*
 * xarray to maintain one passthru subsystem per nvme controller.
 */
static DEFINE_XARRAY(passthru_subsystems);

void nvmet_passthrough_override_cap(struct nvmet_ctrl *ctrl)
{
	/*
	 * Multiple command set support can only be declared if the underlying
	 * controller actually supports it.
	 */
	if (!nvme_multi_css(ctrl->subsys->passthru_ctrl))
		ctrl->cap &= ~(1ULL << 43);
}

static u16 nvmet_passthru_override_id_descs(struct nvmet_req *req)
{
	struct nvmet_ctrl *ctrl = req->sq->ctrl;
	u16 status = NVME_SC_SUCCESS;
	int pos, len;
	bool csi_seen = false;
	void *data;
	u8 csi;

	if (!ctrl->subsys->clear_ids)
		return status;

	data = kzalloc(NVME_IDENTIFY_DATA_SIZE, GFP_KERNEL);
	if (!data)
		return NVME_SC_INTERNAL;

	status = nvmet_copy_from_sgl(req, 0, data, NVME_IDENTIFY_DATA_SIZE);
	if (status)
		goto out_free;

	for (pos = 0; pos < NVME_IDENTIFY_DATA_SIZE; pos += len) {
		struct nvme_ns_id_desc *cur = data + pos;

		if (cur->nidl == 0)
			break;
		if (cur->nidt == NVME_NIDT_CSI) {
			memcpy(&csi, cur + 1, NVME_NIDT_CSI_LEN);
			csi_seen = true;
			break;
		}
		len = sizeof(struct nvme_ns_id_desc) + cur->nidl;
	}

	memset(data, 0, NVME_IDENTIFY_DATA_SIZE);
	if (csi_seen) {
		struct nvme_ns_id_desc *cur = data;

		cur->nidt = NVME_NIDT_CSI;
		cur->nidl = NVME_NIDT_CSI_LEN;
		memcpy(cur + 1, &csi, NVME_NIDT_CSI_LEN);
	}
	status = nvmet_copy_to_sgl(req, 0, data, NVME_IDENTIFY_DATA_SIZE);
out_free:
	kfree(data);
	return status;
}

static u16 nvmet_passthru_override_id_ctrl(struct nvmet_req *req)
{
	struct nvmet_ctrl *ctrl = req->sq->ctrl;
	struct nvme_ctrl *pctrl = ctrl->subsys->passthru_ctrl;
	u16 status = NVME_SC_SUCCESS;
	struct nvme_id_ctrl *id;
	unsigned int max_hw_sectors;
	int page_shift;

	id = kzalloc(sizeof(*id), GFP_KERNEL);
	if (!id)
		return NVME_SC_INTERNAL;

	status = nvmet_copy_from_sgl(req, 0, id, sizeof(*id));
	if (status)
		goto out_free;

	id->cntlid = cpu_to_le16(ctrl->cntlid);
	id->ver = cpu_to_le32(ctrl->subsys->ver);

	/*
	 * The passthru NVMe driver may have a limit on the number of segments
	 * which depends on the host's memory fragmentation. To solve this,
	 * ensure mdts is limited to the pages equal to the number of segments.
	 */
	max_hw_sectors = min_not_zero(pctrl->max_segments << PAGE_SECTORS_SHIFT,
				      pctrl->max_hw_sectors);

	/*
	 * nvmet_passthru_map_sg is limited to using a single bio so limit
	 * the mdts based on BIO_MAX_VECS as well
	 */
	max_hw_sectors = min_not_zero(BIO_MAX_VECS << PAGE_SECTORS_SHIFT,
				      max_hw_sectors);

	page_shift = NVME_CAP_MPSMIN(ctrl->cap) + 12;

	id->mdts = ilog2(max_hw_sectors) + 9 - page_shift;

	id->acl = 3;
	/*
	 * We export aerl limit for the fabrics controller, update this when
	 * passthru based aerl support is added.
	 */
	id->aerl = NVMET_ASYNC_EVENTS - 1;

	/* emulate kas as most of the PCIe ctrl don't have a support for kas */
	id->kas = cpu_to_le16(NVMET_KAS);

	/* don't support host memory buffer */
	id->hmpre = 0;
	id->hmmin = 0;

	id->sqes = min_t(__u8, ((0x6 << 4) | 0x6), id->sqes);
	id->cqes = min_t(__u8, ((0x4 << 4) | 0x4), id->cqes);
	id->maxcmd = cpu_to_le16(NVMET_MAX_CMD(ctrl));

	/* don't support fuse commands */
	id->fuses = 0;

	id->sgls = cpu_to_le32(1 << 0); /* we always support SGLs */
	if (ctrl->ops->flags & NVMF_KEYED_SGLS)
		id->sgls |= cpu_to_le32(1 << 2);
	if (req->port->inline_data_size)
		id->sgls |= cpu_to_le32(1 << 20);

	/*
	 * When passthru controller is setup using nvme-loop transport it will
	 * export the passthru ctrl subsysnqn (PCIe NVMe ctrl) and will fail in
	 * the nvme/host/core.c in the nvme_init_subsystem()->nvme_active_ctrl()
	 * code path with duplicate ctrl subsysnqn. In order to prevent that we
	 * mask the passthru-ctrl subsysnqn with the target ctrl subsysnqn.
	 */
	memcpy(id->subnqn, ctrl->subsysnqn, sizeof(id->subnqn));

	/* use fabric id-ctrl values */
	id->ioccsz = cpu_to_le32((sizeof(struct nvme_command) +
				req->port->inline_data_size) / 16);
	id->iorcsz = cpu_to_le32(sizeof(struct nvme_completion) / 16);

	id->msdbd = ctrl->ops->msdbd;

	/* Support multipath connections with fabrics */
	id->cmic |= 1 << 1;

	/* Disable reservations, see nvmet_parse_passthru_io_cmd() */
	id->oncs &= cpu_to_le16(~NVME_CTRL_ONCS_RESERVATIONS);

	status = nvmet_copy_to_sgl(req, 0, id, sizeof(struct nvme_id_ctrl));

out_free:
	kfree(id);
	return status;
}

static u16 nvmet_passthru_override_id_ns(struct nvmet_req *req)
{
	u16 status = NVME_SC_SUCCESS;
	struct nvme_id_ns *id;
	int i;

	id = kzalloc(sizeof(*id), GFP_KERNEL);
	if (!id)
		return NVME_SC_INTERNAL;

	status = nvmet_copy_from_sgl(req, 0, id, sizeof(struct nvme_id_ns));
	if (status)
		goto out_free;

	for (i = 0; i < (id->nlbaf + 1); i++)
		if (id->lbaf[i].ms)
			memset(&id->lbaf[i], 0, sizeof(id->lbaf[i]));

	id->flbas = id->flbas & ~(1 << 4);

	/*
	 * Presently the NVMEof target code does not support sending
	 * metadata, so we must disable it here. This should be updated
	 * once target starts supporting metadata.
	 */
	id->mc = 0;

	if (req->sq->ctrl->subsys->clear_ids) {
		memset(id->nguid, 0, NVME_NIDT_NGUID_LEN);
		memset(id->eui64, 0, NVME_NIDT_EUI64_LEN);
	}

	status = nvmet_copy_to_sgl(req, 0, id, sizeof(*id));

out_free:
	kfree(id);
	return status;
}

static void nvmet_passthru_execute_cmd_work(struct work_struct *w)
{
	struct nvmet_req *req = container_of(w, struct nvmet_req, p.work);
	struct request *rq = req->p.rq;
	struct nvme_ctrl *ctrl = nvme_req(rq)->ctrl;
	struct nvme_ns *ns = rq->q->queuedata;
	u32 effects;
	int status;

	effects = nvme_passthru_start(ctrl, ns, req->cmd->common.opcode);
	status = nvme_execute_rq(rq, false);
	if (status == NVME_SC_SUCCESS &&
	    req->cmd->common.opcode == nvme_admin_identify) {
		switch (req->cmd->identify.cns) {
		case NVME_ID_CNS_CTRL:
			status = nvmet_passthru_override_id_ctrl(req);
			break;
		case NVME_ID_CNS_NS:
			status = nvmet_passthru_override_id_ns(req);
			break;
		case NVME_ID_CNS_NS_DESC_LIST:
			status = nvmet_passthru_override_id_descs(req);
			break;
		}
	} else if (status < 0)
		status = NVME_SC_INTERNAL;

	req->cqe->result = nvme_req(rq)->result;
	nvmet_req_complete(req, status);
	blk_mq_free_request(rq);

	if (effects)
		nvme_passthru_end(ctrl, ns, effects, req->cmd, status);
}

static enum rq_end_io_ret nvmet_passthru_req_done(struct request *rq,
						  blk_status_t blk_status)
{
	struct nvmet_req *req = rq->end_io_data;

	req->cqe->result = nvme_req(rq)->result;
	nvmet_req_complete(req, nvme_req(rq)->status);
	blk_mq_free_request(rq);
	return RQ_END_IO_NONE;
}

static int nvmet_passthru_map_sg(struct nvmet_req *req, struct request *rq)
{
	struct scatterlist *sg;
	struct bio *bio;
	int ret = -EINVAL;
	int i;

	if (req->sg_cnt > BIO_MAX_VECS)
		return -EINVAL;

	if (nvmet_use_inline_bvec(req)) {
		bio = &req->p.inline_bio;
		bio_init(bio, NULL, req->inline_bvec,
			 ARRAY_SIZE(req->inline_bvec), req_op(rq));
	} else {
		bio = bio_alloc(NULL, bio_max_segs(req->sg_cnt), req_op(rq),
				GFP_KERNEL);
		bio->bi_end_io = bio_put;
	}

	for_each_sg(req->sg, sg, req->sg_cnt, i) {
		if (bio_add_page(bio, sg_page(sg), sg->length, sg->offset) <
				sg->length)
			goto out_bio_put;
	}

	ret = blk_rq_append_bio(rq, bio);
	if (ret)
		goto out_bio_put;
	return 0;

out_bio_put:
	nvmet_req_bio_put(req, bio);
	return ret;
}

static void nvmet_passthru_execute_cmd(struct nvmet_req *req)
{
	struct nvme_ctrl *ctrl = nvmet_req_subsys(req)->passthru_ctrl;
	struct request_queue *q = ctrl->admin_q;
	struct nvme_ns *ns = NULL;
	struct request *rq = NULL;
	unsigned int timeout;
	u32 effects;
	u16 status;
	int ret;

	if (likely(req->sq->qid != 0)) {
		u32 nsid = le32_to_cpu(req->cmd->common.nsid);

		ns = nvme_find_get_ns(ctrl, nsid);
		if (unlikely(!ns)) {
			pr_err("failed to get passthru ns nsid:%u\n", nsid);
			status = NVME_SC_INVALID_NS | NVME_STATUS_DNR;
			goto out;
		}

		q = ns->queue;
		timeout = nvmet_req_subsys(req)->io_timeout;
	} else {
		timeout = nvmet_req_subsys(req)->admin_timeout;
	}

	rq = blk_mq_alloc_request(q, nvme_req_op(req->cmd), 0);
	if (IS_ERR(rq)) {
		status = NVME_SC_INTERNAL;
		goto out_put_ns;
	}
	nvme_init_request(rq, req->cmd);

	if (timeout)
		rq->timeout = timeout;

	if (req->sg_cnt) {
		ret = nvmet_passthru_map_sg(req, rq);
		if (unlikely(ret)) {
			status = NVME_SC_INTERNAL;
			goto out_put_req;
		}
	}

	/*
	 * If a command needs post-execution fixups, or there are any
	 * non-trivial effects, make sure to execute the command synchronously
	 * in a workqueue so that nvme_passthru_end gets called.
	 */
	effects = nvme_command_effects(ctrl, ns, req->cmd->common.opcode);
	if (req->p.use_workqueue ||
	    (effects & ~(NVME_CMD_EFFECTS_CSUPP | NVME_CMD_EFFECTS_LBCC))) {
		INIT_WORK(&req->p.work, nvmet_passthru_execute_cmd_work);
		req->p.rq = rq;
		queue_work(nvmet_wq, &req->p.work);
	} else {
		rq->end_io = nvmet_passthru_req_done;
		rq->end_io_data = req;
		blk_execute_rq_nowait(rq, false);
	}

	if (ns)
		nvme_put_ns(ns);

	return;

out_put_req:
	blk_mq_free_request(rq);
out_put_ns:
	if (ns)
		nvme_put_ns(ns);
out:
	nvmet_req_complete(req, status);
}

/*
 * We need to emulate set host behaviour to ensure that any requested
 * behaviour of the target's host matches the requested behaviour
 * of the device's host and fail otherwise.
 */
static void nvmet_passthru_set_host_behaviour(struct nvmet_req *req)
{
	struct nvme_ctrl *ctrl = nvmet_req_subsys(req)->passthru_ctrl;
	struct nvme_feat_host_behavior *host;
	u16 status = NVME_SC_INTERNAL;
	int ret;

	host = kzalloc(sizeof(*host) * 2, GFP_KERNEL);
	if (!host)
		goto out_complete_req;

	ret = nvme_get_features(ctrl, NVME_FEAT_HOST_BEHAVIOR, 0,
				host, sizeof(*host), NULL);
	if (ret)
		goto out_free_host;

	status = nvmet_copy_from_sgl(req, 0, &host[1], sizeof(*host));
	if (status)
		goto out_free_host;

	if (memcmp(&host[0], &host[1], sizeof(host[0]))) {
		pr_warn("target host has requested different behaviour from the local host\n");
		status = NVME_SC_INTERNAL;
	}

out_free_host:
	kfree(host);
out_complete_req:
	nvmet_req_complete(req, status);
}

static u16 nvmet_setup_passthru_command(struct nvmet_req *req)
{
	req->p.use_workqueue = false;
	req->execute = nvmet_passthru_execute_cmd;
	return NVME_SC_SUCCESS;
}

u16 nvmet_parse_passthru_io_cmd(struct nvmet_req *req)
{
	/* Reject any commands with non-sgl flags set (ie. fused commands) */
	if (req->cmd->common.flags & ~NVME_CMD_SGL_ALL)
		return NVME_SC_INVALID_FIELD;

	switch (req->cmd->common.opcode) {
	case nvme_cmd_resv_register:
	case nvme_cmd_resv_report:
	case nvme_cmd_resv_acquire:
	case nvme_cmd_resv_release:
		/*
		 * Reservations cannot be supported properly because the
		 * underlying device has no way of differentiating different
		 * hosts that connect via fabrics. This could potentially be
		 * emulated in the future if regular targets grow support for
		 * this feature.
		 */
		return NVME_SC_INVALID_OPCODE | NVME_STATUS_DNR;
	}

	return nvmet_setup_passthru_command(req);
}

/*
 * Only features that are emulated or specifically allowed in the list  are
 * passed down to the controller. This function implements the allow list for
 * both get and set features.
 */
static u16 nvmet_passthru_get_set_features(struct nvmet_req *req)
{
	switch (le32_to_cpu(req->cmd->features.fid)) {
	case NVME_FEAT_ARBITRATION:
	case NVME_FEAT_POWER_MGMT:
	case NVME_FEAT_LBA_RANGE:
	case NVME_FEAT_TEMP_THRESH:
	case NVME_FEAT_ERR_RECOVERY:
	case NVME_FEAT_VOLATILE_WC:
	case NVME_FEAT_WRITE_ATOMIC:
	case NVME_FEAT_AUTO_PST:
	case NVME_FEAT_TIMESTAMP:
	case NVME_FEAT_HCTM:
	case NVME_FEAT_NOPSC:
	case NVME_FEAT_RRL:
	case NVME_FEAT_PLM_CONFIG:
	case NVME_FEAT_PLM_WINDOW:
	case NVME_FEAT_HOST_BEHAVIOR:
	case NVME_FEAT_SANITIZE:
	case NVME_FEAT_VENDOR_START ... NVME_FEAT_VENDOR_END:
		return nvmet_setup_passthru_command(req);

	case NVME_FEAT_ASYNC_EVENT:
		/* There is no support for forwarding ASYNC events */
	case NVME_FEAT_IRQ_COALESCE:
	case NVME_FEAT_IRQ_CONFIG:
		/* The IRQ settings will not apply to the target controller */
	case NVME_FEAT_HOST_MEM_BUF:
		/*
		 * Any HMB that's set will not be passed through and will
		 * not work as expected
		 */
	case NVME_FEAT_SW_PROGRESS:
		/*
		 * The Pre-Boot Software Load Count doesn't make much
		 * sense for a target to export
		 */
	case NVME_FEAT_RESV_MASK:
	case NVME_FEAT_RESV_PERSIST:
		/* No reservations, see nvmet_parse_passthru_io_cmd() */
	default:
		return NVME_SC_INVALID_OPCODE | NVME_STATUS_DNR;
	}
}

u16 nvmet_parse_passthru_admin_cmd(struct nvmet_req *req)
{
	/* Reject any commands with non-sgl flags set (ie. fused commands) */
	if (req->cmd->common.flags & ~NVME_CMD_SGL_ALL)
		return NVME_SC_INVALID_FIELD;

	/*
	 * Passthru all vendor specific commands
	 */
	if (req->cmd->common.opcode >= nvme_admin_vendor_start)
		return nvmet_setup_passthru_command(req);

	switch (req->cmd->common.opcode) {
	case nvme_admin_async_event:
		req->execute = nvmet_execute_async_event;
		return NVME_SC_SUCCESS;
	case nvme_admin_keep_alive:
		/*
		 * Most PCIe ctrls don't support keep alive cmd, we route keep
		 * alive to the non-passthru mode. In future please change this
		 * code when PCIe ctrls with keep alive support available.
		 */
		req->execute = nvmet_execute_keep_alive;
		return NVME_SC_SUCCESS;
	case nvme_admin_set_features:
		switch (le32_to_cpu(req->cmd->features.fid)) {
		case NVME_FEAT_ASYNC_EVENT:
		case NVME_FEAT_KATO:
		case NVME_FEAT_NUM_QUEUES:
		case NVME_FEAT_HOST_ID:
			req->execute = nvmet_execute_set_features;
			return NVME_SC_SUCCESS;
		case NVME_FEAT_HOST_BEHAVIOR:
			req->execute = nvmet_passthru_set_host_behaviour;
			return NVME_SC_SUCCESS;
		default:
			return nvmet_passthru_get_set_features(req);
		}
		break;
	case nvme_admin_get_features:
		switch (le32_to_cpu(req->cmd->features.fid)) {
		case NVME_FEAT_ASYNC_EVENT:
		case NVME_FEAT_KATO:
		case NVME_FEAT_NUM_QUEUES:
		case NVME_FEAT_HOST_ID:
			req->execute = nvmet_execute_get_features;
			return NVME_SC_SUCCESS;
		default:
			return nvmet_passthru_get_set_features(req);
		}
		break;
	case nvme_admin_identify:
		switch (req->cmd->identify.cns) {
		case NVME_ID_CNS_CS_CTRL:
			switch (req->cmd->identify.csi) {
			case NVME_CSI_ZNS:
				req->execute = nvmet_passthru_execute_cmd;
				req->p.use_workqueue = true;
				return NVME_SC_SUCCESS;
			}
			return NVME_SC_INVALID_OPCODE | NVME_STATUS_DNR;
		case NVME_ID_CNS_CTRL:
		case NVME_ID_CNS_NS:
		case NVME_ID_CNS_NS_DESC_LIST:
			req->execute = nvmet_passthru_execute_cmd;
			req->p.use_workqueue = true;
			return NVME_SC_SUCCESS;
		case NVME_ID_CNS_CS_NS:
			switch (req->cmd->identify.csi) {
			case NVME_CSI_ZNS:
				req->execute = nvmet_passthru_execute_cmd;
				req->p.use_workqueue = true;
				return NVME_SC_SUCCESS;
			}
			return NVME_SC_INVALID_OPCODE | NVME_STATUS_DNR;
		default:
			return nvmet_setup_passthru_command(req);
		}
	case nvme_admin_get_log_page:
		return nvmet_setup_passthru_command(req);
	default:
		/* Reject commands not in the allowlist above */
		return nvmet_report_invalid_opcode(req);
	}
}

int nvmet_passthru_ctrl_enable(struct nvmet_subsys *subsys)
{
	struct nvme_ctrl *ctrl;
	struct file *file;
	int ret = -EINVAL;
	void *old;

	mutex_lock(&subsys->lock);
	if (!subsys->passthru_ctrl_path)
		goto out_unlock;
	if (subsys->passthru_ctrl)
		goto out_unlock;

	if (subsys->nr_namespaces) {
		pr_info("cannot enable both passthru and regular namespaces for a single subsystem");
		goto out_unlock;
	}

	file = filp_open(subsys->passthru_ctrl_path, O_RDWR, 0);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		goto out_unlock;
	}

	ctrl = nvme_ctrl_from_file(file);
	if (!ctrl) {
		pr_err("failed to open nvme controller %s\n",
		       subsys->passthru_ctrl_path);

		goto out_put_file;
	}

	old = xa_cmpxchg(&passthru_subsystems, ctrl->instance, NULL,
			 subsys, GFP_KERNEL);
	if (xa_is_err(old)) {
		ret = xa_err(old);
		goto out_put_file;
	}

	if (old)
		goto out_put_file;

	subsys->passthru_ctrl = ctrl;
	subsys->ver = ctrl->vs;

	if (subsys->ver < NVME_VS(1, 2, 1)) {
		pr_warn("nvme controller version is too old: %llu.%llu.%llu, advertising 1.2.1\n",
			NVME_MAJOR(subsys->ver), NVME_MINOR(subsys->ver),
			NVME_TERTIARY(subsys->ver));
		subsys->ver = NVME_VS(1, 2, 1);
	}
	nvme_get_ctrl(ctrl);
	__module_get(subsys->passthru_ctrl->ops->module);
	ret = 0;

out_put_file:
	filp_close(file, NULL);
out_unlock:
	mutex_unlock(&subsys->lock);
	return ret;
}

static void __nvmet_passthru_ctrl_disable(struct nvmet_subsys *subsys)
{
	if (subsys->passthru_ctrl) {
		xa_erase(&passthru_subsystems, subsys->passthru_ctrl->instance);
		module_put(subsys->passthru_ctrl->ops->module);
		nvme_put_ctrl(subsys->passthru_ctrl);
	}
	subsys->passthru_ctrl = NULL;
	subsys->ver = NVMET_DEFAULT_VS;
}

void nvmet_passthru_ctrl_disable(struct nvmet_subsys *subsys)
{
	mutex_lock(&subsys->lock);
	__nvmet_passthru_ctrl_disable(subsys);
	mutex_unlock(&subsys->lock);
}

void nvmet_passthru_subsys_free(struct nvmet_subsys *subsys)
{
	mutex_lock(&subsys->lock);
	__nvmet_passthru_ctrl_disable(subsys);
	mutex_unlock(&subsys->lock);
	kfree(subsys->passthru_ctrl_path);
}
