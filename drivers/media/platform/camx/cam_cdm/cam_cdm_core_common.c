// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2017-2019, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/module.h>
#include <linux/iommu.h>
#include <linux/timer.h>
#include <linux/kernel.h>

#include "cam_soc_util.h"
#include "cam_smmu_api.h"
#include "cam_io_util.h"
#include "cam_cdm_intf_api.h"
#include "cam_cdm.h"
#include "cam_cdm_soc.h"
#include "cam_cdm_core_common.h"

static void client_final_put(struct kref *kref)
{
	struct cam_cdm_client *client;

	client = container_of(kref, struct cam_cdm_client, kref);
	kfree(client);
}

static bool cam_cdm_get_client(struct cam_cdm_client *client)
{
	return kref_get_unless_zero(&client->kref);
}

static void cam_cdm_put_client(struct cam_cdm_client *client)
{
	kref_put(&client->kref, client_final_put);
}

struct cam_cdm_client *cam_cdm_lookup_client(struct cam_cdm *cdm, u32 idx)
{
	struct cam_cdm_client *client = NULL;
	unsigned long flags;

	read_lock_irqsave(&cdm->clients_lock, flags);
	if (cdm->clients[idx] && cam_cdm_get_client(cdm->clients[idx]))
		client = cdm->clients[idx];
	read_unlock_irqrestore(&cdm->clients_lock, flags);

	return client;
}

int cam_cdm_insert_client(struct cam_cdm *cdm, struct cam_cdm_client *client)
{
	unsigned long flags;
	int ret = -EINVAL;
	u32 idx;

	/* We should be able to grab ownership */
	if (!cam_cdm_get_client(client))
		return ret;

	write_lock_irqsave(&cdm->clients_lock, flags);
	for (idx = 0; idx < CAM_PER_CDM_MAX_REGISTERED_CLIENTS; idx++) {
		if (cdm->clients[idx])
			continue;

		/* Found available slot */
		client->handle = CAM_CDM_CREATE_CLIENT_HANDLE(cdm->index, idx);
		cdm->clients[idx] = client;
		ret = 0;
		break;
	}
	write_unlock_irqrestore(&cdm->clients_lock, flags);

	/*
	 * Did not find the slot, drop the refcount because we don't own
	 * the object
	 */
	if (ret)
		cam_cdm_put_client(client);
	return ret;
}

void cam_cdm_remove_client(struct cam_cdm *cdm, u32 idx)
{
	struct cam_cdm_client *client;
	unsigned long flags;

	/* Remove from cdm clients table */
	write_lock_irqsave(&cdm->clients_lock, flags);
	client = cdm->clients[idx];
	cdm->clients[idx] = NULL;
	write_unlock_irqrestore(&cdm->clients_lock, flags);

	/* Drop cdm ownership refcount */
	if (client)
		cam_cdm_put_client(client);
}

bool cam_cdm_set_cam_hw_version(
	uint32_t ver, struct cam_hw_version *cam_version)
{
	switch (ver) {
	case CAM_CDM170_VERSION:
	case CAM_CDM175_VERSION:
		cam_version->major    = (ver & 0xF0000000);
		cam_version->minor    = (ver & 0xFFF0000);
		cam_version->incr     = (ver & 0xFFFF);
		cam_version->reserved = 0;
		return true;
	default:
		CAM_ERR(CAM_CDM, "CDM Version=%x not supported in util", ver);
	break;
	}
	return false;
}

bool cam_cdm_cpas_cb(uint32_t client_handle, void *userdata,
	struct cam_cpas_irq_data *irq_data)
{
	if (!irq_data)
		return false;

	CAM_DBG(CAM_CDM, "CPAS error callback type=%d", irq_data->irq_type);

	return false;
}

const
struct cam_cdm_utils_ops *cam_cdm_get_ops(u32 ver,
					  struct cam_hw_version *cam_version,
					  bool by_cam_version)
{
	if (by_cam_version == false) {
		switch (ver) {
		case CAM_CDM170_VERSION:
		case CAM_CDM175_VERSION:
			return cam_cdm_util_get_cmd170_ops();
		default:
			CAM_ERR(CAM_CDM, "CDM Version=%x not supported in util",
				ver);
		}
	} else if (cam_version) {
		if (((cam_version->major == 1) &&
			(cam_version->minor == 0) &&
			(cam_version->incr == 0)) ||
			((cam_version->major == 1) &&
			(cam_version->minor == 1) &&
			(cam_version->incr == 0))) {

			CAM_DBG(CAM_CDM,
				"cam_hw_version=%x:%x:%x supported",
				cam_version->major, cam_version->minor,
				cam_version->incr);
			return cam_cdm_util_get_cmd170_ops();
		}

		CAM_ERR(CAM_CDM, "cam_hw_version=%x:%x:%x not supported",
			cam_version->major, cam_version->minor,
			cam_version->incr);
	}

	return NULL;
}

struct cam_cdm_bl_cb_request_entry *cam_cdm_find_request_by_bl_tag(
	uint32_t tag, struct list_head *bl_list)
{
	struct cam_cdm_bl_cb_request_entry *node;

	list_for_each_entry(node, bl_list, entry) {
		if (node->bl_tag == tag)
			return node;
	}
	CAM_ERR(CAM_CDM, "Could not find the bl request for tag=%x", tag);

	return NULL;
}

int cam_cdm_get_caps(void *hw_priv,
	void *get_hw_cap_args, uint32_t arg_size)
{
	struct cam_hw_info *cdm_hw = hw_priv;
	struct cam_cdm *cdm_core;

	if ((cdm_hw) && (cdm_hw->core_info) && (get_hw_cap_args) &&
		(sizeof(struct cam_iommu_handle) == arg_size)) {
		cdm_core = (struct cam_cdm *)cdm_hw->core_info;
		*((struct cam_iommu_handle *)get_hw_cap_args) =
			cdm_core->iommu_hdl;
		return 0;
	}

	return -EINVAL;
}

void cam_cdm_notify_clients(struct cam_hw_info *cdm_hw,
	enum cam_cdm_cb_status status, void *data)
{
	int i;
	struct cam_cdm *core = NULL;
	struct cam_cdm_client *client = NULL;

	if (!cdm_hw) {
		CAM_ERR(CAM_CDM, "CDM Notify called with NULL hw info");
		return;
	}
	core = (struct cam_cdm *)cdm_hw->core_info;

	if (status == CAM_CDM_CB_STATUS_BL_SUCCESS) {
		int client_idx;
		struct cam_cdm_bl_cb_request_entry *node =
			(struct cam_cdm_bl_cb_request_entry *)data;

		client_idx = CAM_CDM_GET_CLIENT_IDX(node->client_hdl);
		client = cam_cdm_lookup_client(core, client_idx);
		if (!client)
			return;

		if (client->handle != node->client_hdl) {
			CAM_ERR(CAM_CDM, "Invalid client %pK hdl=%x", client,
				node->client_hdl);
			cam_cdm_put_client(client);
			return;
		}
		if (client->data.cam_cdm_callback) {
			CAM_DBG(CAM_CDM, "Calling client=%s cb cookie=%d",
				client->data.identifier, node->cookie);
			client->data.cam_cdm_callback(node->client_hdl,
				node->userdata, CAM_CDM_CB_STATUS_BL_SUCCESS,
				node->cookie);
			CAM_DBG(CAM_CDM, "Exit client cb cookie=%d",
				node->cookie);
		} else {
			CAM_ERR(CAM_CDM, "No cb registered for client hdl=%x",
				node->client_hdl);
		}
		cam_cdm_put_client(client);
		return;
	}

	/*
	 * We don't hold clients_lock here, the lookup acquires it when
	 * needed
	 */
	for (i = 0; i < CAM_PER_CDM_MAX_REGISTERED_CLIENTS; i++) {
		client = cam_cdm_lookup_client(core, i);
		if (!client)
			continue;

		CAM_DBG(CAM_CDM, "Found client slot %d", i);
		if (client->data.cam_cdm_callback) {
			if (status == CAM_CDM_CB_STATUS_PAGEFAULT) {
				unsigned long iova = (unsigned long)data;

				client->data.cam_cdm_callback(client->handle,
						client->data.userdata,
						CAM_CDM_CB_STATUS_PAGEFAULT,
						(iova & 0xFFFFFFFF));
			} else {
				CAM_ERR(CAM_CDM,
					"No cb registered for client hdl=%x",
					client->handle);
			}
		}
		cam_cdm_put_client(client);
	}
}

int cam_cdm_stream_ops_internal(void *hw_priv,
	void *start_args, bool operation)
{
	struct cam_hw_info *cdm_hw = hw_priv;
	struct cam_cdm *core = NULL;
	int rc = -EPERM;
	int client_idx;
	struct cam_cdm_client *client;
	uint32_t *handle = start_args;

	if (!hw_priv)
		return -EINVAL;

	core = (struct cam_cdm *)cdm_hw->core_info;
	client_idx = CAM_CDM_GET_CLIENT_IDX(*handle);

	client = cam_cdm_lookup_client(core, client_idx);
	if (!client)
		return -EINVAL;

	if (*handle != client->handle) {
		CAM_ERR(CAM_CDM, "client id given handle=%x invalid", *handle);
		cam_cdm_put_client(client);
		return -EINVAL;
	}

	if (operation == true) {
		if (true == client->stream_on) {
			CAM_ERR(CAM_CDM,
				"Invalid CDM client is already streamed ON");
			cam_cdm_put_client(client);
			return rc;
		}
	} else {
		if (client->stream_on == false) {
			CAM_ERR(CAM_CDM,
				"Invalid CDM client is already streamed Off");
			cam_cdm_put_client(client);
			return rc;
		}
	}

	mutex_lock(&cdm_hw->hw_mutex);
	if (operation == true) {
		if (!cdm_hw->open_count) {
			struct cam_ahb_vote ahb_vote;
			struct cam_axi_vote axi_vote;

			ahb_vote.type = CAM_VOTE_ABSOLUTE;
			ahb_vote.vote.level = CAM_SVS_VOTE;
			axi_vote.compressed_bw = CAM_CPAS_DEFAULT_AXI_BW;
			axi_vote.compressed_bw_ab = CAM_CPAS_DEFAULT_AXI_BW;
			axi_vote.uncompressed_bw = CAM_CPAS_DEFAULT_AXI_BW;

			rc = cam_cpas_start(core->cpas_handle,
				&ahb_vote, &axi_vote);
			if (rc != 0) {
				CAM_ERR(CAM_CDM, "CPAS start failed");
				goto end;
			}
			CAM_DBG(CAM_CDM, "CDM init first time");
			if (core->id == CAM_CDM_VIRTUAL) {
				CAM_DBG(CAM_CDM,
					"Virtual CDM HW init first time");
				rc = 0;
			} else {
				CAM_DBG(CAM_CDM, "CDM HW init first time");
				rc = cam_hw_cdm_init(hw_priv, NULL, 0);
				if (rc == 0) {
					rc = cam_hw_cdm_alloc_genirq_mem(
						hw_priv);
					if (rc != 0) {
						CAM_ERR(CAM_CDM,
							"Genirqalloc failed");
						cam_hw_cdm_deinit(hw_priv,
							NULL, 0);
					}
				} else {
					CAM_ERR(CAM_CDM, "CDM HW init failed");
				}
			}
			if (rc == 0) {
				cdm_hw->open_count++;
				client->stream_on = true;
			} else {
				if (cam_cpas_stop(core->cpas_handle))
					CAM_ERR(CAM_CDM, "CPAS stop failed");
			}
		} else {
			cdm_hw->open_count++;
			CAM_DBG(CAM_CDM, "CDM HW already ON count=%d",
				cdm_hw->open_count);
			rc = 0;
			client->stream_on = true;
		}
	} else {
		if (cdm_hw->open_count) {
			cdm_hw->open_count--;
			CAM_DBG(CAM_CDM, "stream OFF CDM %d",
				cdm_hw->open_count);
			if (!cdm_hw->open_count) {
				CAM_DBG(CAM_CDM, "CDM Deinit now");
				if (core->id == CAM_CDM_VIRTUAL) {
					CAM_DBG(CAM_CDM,
						"Virtual CDM HW Deinit");
					rc = 0;
				} else {
					CAM_DBG(CAM_CDM, "CDM HW Deinit now");
					rc = cam_hw_cdm_deinit(
						hw_priv, NULL, 0);
					if (cam_hw_cdm_release_genirq_mem(
						hw_priv))
						CAM_ERR(CAM_CDM,
							"Genirq release fail");
				}
				if (rc) {
					CAM_ERR(CAM_CDM,
						"Deinit failed in streamoff");
				} else {
					client->stream_on = false;
					rc = cam_cpas_stop(core->cpas_handle);
					if (rc)
						CAM_ERR(CAM_CDM,
							"CPAS stop failed");
				}
			} else {
				client->stream_on = false;
				rc = 0;
				CAM_DBG(CAM_CDM,
					"Client stream off success =%d",
					cdm_hw->open_count);
			}
		} else {
			CAM_DBG(CAM_CDM, "stream OFF CDM Invalid %d",
				cdm_hw->open_count);
			rc = -ENXIO;
		}
	}
end:
	cam_cdm_put_client(client);
	mutex_unlock(&cdm_hw->hw_mutex);
	return rc;
}

int cam_cdm_stream_start(void *hw_priv,
	void *start_args, uint32_t size)
{
	int rc = 0;

	if (!hw_priv)
		return -EINVAL;

	rc = cam_cdm_stream_ops_internal(hw_priv, start_args, true);
	return rc;

}

int cam_cdm_stream_stop(void *hw_priv,
	void *start_args, uint32_t size)
{
	int rc = 0;

	if (!hw_priv)
		return -EINVAL;

	rc = cam_cdm_stream_ops_internal(hw_priv, start_args, false);
	return rc;

}

int cam_cdm_process_cmd(void *hw_priv,
	uint32_t cmd, void *cmd_args, uint32_t arg_size)
{
	struct cam_hw_info *cdm_hw = hw_priv;
	struct cam_cdm_client *client;
	struct cam_cdm *core = NULL;
	int rc = -EINVAL;
	int idx;

	if ((!hw_priv) || (!cmd_args) ||
		(cmd >= CAM_CDM_HW_INTF_CMD_INVALID))
		return rc;

	core = (struct cam_cdm *)cdm_hw->core_info;
	switch (cmd) {
	case CAM_CDM_HW_INTF_CMD_SUBMIT_BL: {
		struct cam_cdm_hw_intf_cmd_submit_bl *req;

		if (sizeof(struct cam_cdm_hw_intf_cmd_submit_bl) != arg_size) {
			CAM_ERR(CAM_CDM, "Invalid CDM cmd %d arg size=%x", cmd,
				arg_size);
			break;
		}

		req = (struct cam_cdm_hw_intf_cmd_submit_bl *)cmd_args;
		if ((req->data->type < 0) ||
			(req->data->type > CAM_CDM_BL_CMD_TYPE_HW_IOVA)) {
			CAM_ERR(CAM_CDM, "Invalid req bl cmd addr type=%d",
				req->data->type);
			break;
		}

		idx = CAM_CDM_GET_CLIENT_IDX(req->handle);
		client = cam_cdm_lookup_client(core, idx);
		if (!client)
			break;

		if (req->handle != client->handle) {
			cam_cdm_put_client(client);
			break;
		}

		if ((req->data->flag == true) &&
			(!client->data.cam_cdm_callback)) {
			CAM_ERR(CAM_CDM,
				"CDM request cb without registering cb");
			cam_cdm_put_client(client);
			break;
		}

		if (client->stream_on != true) {
			CAM_ERR(CAM_CDM,
				"Invalid CDM needs to be streamed ON first");
			cam_cdm_put_client(client);
			break;
		}

		if (core->id == CAM_CDM_VIRTUAL)
			rc = cam_virtual_cdm_submit_bl(cdm_hw, req, client);
		else
			rc = cam_hw_cdm_submit_bl(cdm_hw, req, client);

		cam_cdm_put_client(client);
		break;
	}
	case CAM_CDM_HW_INTF_CMD_ACQUIRE: {
		struct cam_cdm_acquire_data *data;

		if (sizeof(struct cam_cdm_acquire_data) != arg_size) {
			CAM_ERR(CAM_CDM, "Invalid CDM cmd %d arg size=%x", cmd,
				arg_size);
			break;
		}

		client = kzalloc(sizeof(*client), GFP_KERNEL);
		if (!client) {
			rc = -ENOMEM;
			break;
		}

		mutex_lock(&cdm_hw->hw_mutex);
		data = (struct cam_cdm_acquire_data *)cmd_args;
		CAM_DBG(CAM_CDM, "Trying to acquire client=%s in hw idx=%d",
			data->identifier, core->index);

		data->ops = core->ops;
		if (core->id == CAM_CDM_VIRTUAL) {
			data->cdm_version.major = 1;
			data->cdm_version.minor = 0;
			data->cdm_version.incr = 0;
			data->cdm_version.reserved = 0;
			data->ops = cam_cdm_get_ops(0,
					&data->cdm_version, true);
			if (!data->ops) {
				mutex_unlock(&cdm_hw->hw_mutex);
				kfree(client);
				rc = -EPERM;
				CAM_ERR(CAM_CDM, "Invalid ops for virtual cdm");
				break;
			}
		} else {
			data->cdm_version = core->version;
		}

		mutex_init(&client->lock);
		/* Sets kref to 1, this is "our" local ownership */
		kref_init(&client->kref);
		/* Complete cient initialization */
		memcpy(&client->data, data,
		       sizeof(struct cam_cdm_acquire_data));
		client->stream_on = false;
		CAM_DBG(CAM_CDM, "Acquired client=%s in hwidx=%d",
			data->identifier, core->index);

		/* Now insert it and grab cdm ownership */
		rc = cam_cdm_insert_client(core, client);
		/* If client inserted, copy out its handle */
		if (!rc)
			data->handle = client->handle;
		mutex_unlock(&cdm_hw->hw_mutex);

		/*
		 * Drop "our" ownership. cdm table holds the refcount, if
		 * not (insert has failed) then this is "final" put.
		 */
		cam_cdm_put_client(client);
		break;
	}
	case CAM_CDM_HW_INTF_CMD_RELEASE: {
		uint32_t *handle = cmd_args;

		if (sizeof(uint32_t) != arg_size) {
			CAM_ERR(CAM_CDM,
				"Invalid CDM cmd %d size=%x for handle=%x",
				cmd, arg_size, *handle);
			return -EINVAL;
		}
		idx = CAM_CDM_GET_CLIENT_IDX(*handle);
		mutex_lock(&cdm_hw->hw_mutex);

		client = cam_cdm_lookup_client(core, idx);
		if (!client) {
			mutex_unlock(&cdm_hw->hw_mutex);
			break;
		}

		if (*handle != client->handle) {
			CAM_ERR(CAM_CDM, "Invalid client %pK hdl=%x",
				client, *handle);
			mutex_unlock(&cdm_hw->hw_mutex);
			cam_cdm_put_client(client);
			break;
		}

		cam_cdm_remove_client(core, idx);
		/* Drop lookup refcount, this can be the final put */
		cam_cdm_put_client(client);

		mutex_unlock(&cdm_hw->hw_mutex);
		rc = 0;
		break;
	}
	case CAM_CDM_HW_INTF_CMD_RESET_HW: {
		CAM_ERR(CAM_CDM, "CDM HW reset not supported for handle =%x",
			*((uint32_t *)cmd_args));
		break;
	}
	default:
		CAM_ERR(CAM_CDM, "CDM HW intf command not valid =%d", cmd);
		break;
	}
	return rc;
}
