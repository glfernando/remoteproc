/*
 * Remote processor messaging
 *
 * Copyright(c) 2012 Texas Instruments. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in
 *   the documentation and/or other materials provided with the
 *   distribution.
 * * Neither the name Texas Instruments nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _LINUX_RPMSG_RESMGR_H
#define _LINUX_RPMSG_RESMGR_H

struct rprm_manager;

/*
 * enum rprm_action - RPMSG resource manager actions
 * @RPRM_REQUEST:	resource request from the client.
 * @RPRM_RELEASE:	resource release from the client.
 *
 */
enum rprm_action {
	RPRM_REQUEST		= 0,
	RPRM_RELEASE		= 1,
};

/**
 * struct rprm_res - resource manager resource structure
 * @name:	name of the resource
 * @request:	request a resource (mandatory)
 * @release:	release a resource  (mandatory)
 * @get_info:	get info about the resource into a buffer (optional)
 */
struct rprm_res {
	const char *name;
	int (*request)(struct rprm_manager *mgr, void **handle,
						 void *args, size_t len);
	int (*release)(struct rprm_manager *mgr, void *handle);
	int (*get_info)(struct rprm_manager *mgr, void *handle,
						 char *buf, size_t len);
};

/**
 * struct rprm_manager - a specific resource manager structure for a rproc
 * @next:	pointer to the next manager
 * @name:	name of the manager
 * @ower:	module owner of the manager
 * @dev:	device associated with the manager
 * @resources:	resources supported by the manager
 * @res_cnt:	number of resources supported by the manager
 * @dentry:	debugfs entry for debugging
 */
struct rprm_manager {
	struct list_head next;
	const char *name;
	struct module *owner;
	struct device *dev;
	struct rprm_res *resources;
	unsigned res_cnt;
	struct dentry *dentry;
};

/**
 * struct rprm_request - header of a request action
 * @idx:	resource index
 * @data:	additional information needed by below layer (parameters)
 */
struct rprm_request {
	u32 idx;
	char data[];
} __packed;

/**
 * struct rprm_release - header of a release action
 * @res_id:	id of the resource
 */
struct rprm_release {
	u32 res_id;
} __packed;

/**
 * struct rprm_msg - header for all the actions
 * @action:	action requested
 * @data:	addition information depending on @action
 */
struct rprm_msg {
	u32 action;
	char data[];
} __packed;

/**
 * struct rprm request_ack - header for respond and action
 * @res_id:	resource id
 * @base:	resource base address (da address of the resource)
 * @data:	additional information returned to the client
 */
struct rprm_request_ack {
	u32 res_id;
	u32 base;
	char data[];
} __packed;

/**
 * struct rprm_ack - header for acknowledge of a request
 * @action:	action requested
 * @ret:	status value returned by rmsg resmgr server (kernel status)
 * @data:	additional information returned to the client
 */
struct rprm_ack {
	u32 action;
	u32 ret;
	char data[];
} __packed;

int rprm_manager_register(struct rprm_manager *mgr);
int rprm_manager_unregister(struct rprm_manager *mgr);
#endif /* _LINUX_RPMSG_RESMGR_H */
