/*
 *  The OpenDiamond Platform for Interactive Search
 *  Version 3
 *
 *  Copyright (c) 2002-2007 Intel Corporation
 *  Copyright (c) 2006 Larry Huston <larry@thehustons.net>
 *  Copyright (c) 2006-2008 Carnegie Mellon University
 *  All rights reserved.
 *
 *  This software is distributed under the terms of the Eclipse Public
 *  License, Version 1.0 which can be found in the file named LICENSE.
 *  ANY USE, REPRODUCTION OR DISTRIBUTION OF THIS SOFTWARE CONSTITUTES
 *  RECIPIENT'S ACCEPTANCE OF THIS AGREEMENT
 */

/*
 * These file handles a lot of the device specific code.  For the current
 * version we have state for each of the devices.
 */
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/time.h>
#include <netdb.h>
#include <string.h>
#include <assert.h>
#include "diamond_consts.h"
#include "diamond_types.h"
#include "lib_tools.h"
#include "obj_attr.h"
#include "lib_odisk.h"
#include "socket_trans.h"
#include "lib_dctl.h"
#include "lib_auth.h"
#include "lib_sstub.h"
#include "sstub_impl.h"
#include "ports.h"


/*
 * XXX do we manage the complete ring also?? 
 */
float
sstub_get_drate(void *cookie)
{

	cstate_t       *cstate;
	cstate = (cstate_t *) cookie;

	return (ring_drate(cstate->partial_obj_ring));
}


void sstub_get_conn_info(void *cookie, session_info_t *cinfo) {

	cstate_t       *cstate;

	cstate = (cstate_t *) cookie;
	memcpy((void *)cinfo, (void *)&cstate->cinfo, sizeof(session_info_t));
	
	return;
}


/*
 * Send an object. 
 *
 * return current queue depth??
 */
int
sstub_send_obj(void *cookie, obj_data_t * obj, int ver_no, int complete)
{
	obj_info_t	*oi;
	cstate_t	*cstate;
	int             err;

	cstate = (cstate_t *) cookie;

	oi = malloc(sizeof(*oi));
	if (!oi) return ENOMEM;

	oi->obj = obj;
	oi->ver_num = ver_no;

	/*
	 * Set a flag to indicate there is object
	 * data associated with our connection.
	 */
	/*
	 * XXX log 
	 */
	pthread_mutex_lock(&cstate->cmutex);
	cstate->flags |= CSTATE_OBJ_DATA;

	if (complete) {
		err = ring_enq(cstate->complete_obj_ring, oi);
	} else {
		err = ring_enq(cstate->partial_obj_ring, oi);
	}
	pthread_mutex_unlock(&cstate->cmutex);

	if (err) {
		/*
		 * XXX log 
		 */
		/*
		 * XXX how do we handle this 
		 */
		return (err);
	}

	return (0);
}

int
sstub_get_partial(void *cookie, obj_data_t **obj)
{
	obj_info_t	*oi;
	cstate_t	*cstate;
	int             err;

	cstate = (cstate_t *) cookie;

	/*
	 * Set a flag to indicate there is object
	 * data associated with our connection.
	 */
	/*
	 * XXX log 
	 */
	pthread_mutex_lock(&cstate->cmutex);
	oi = ring_deq(cstate->partial_obj_ring);
	pthread_mutex_unlock(&cstate->cmutex);

	if (!oi)
		return 1;

	*obj = oi->obj;
	free(oi);
	return 0;
}

int
sstub_flush_objs(void *cookie, int ver_no)
{
	obj_info_t	*oi;
	cstate_t	*cstate;
	int             err;
	obj_data_t	*obj;
	listener_state_t *lstate;

	cstate = (cstate_t *) cookie;
	lstate = cstate->lstate;

	/*
	 * Set a flag to indicate there is object
	 * data associated with our connection.
	 */
	/*
	 * XXX log 
	 */
	while (1) {
		pthread_mutex_lock(&cstate->cmutex);
		oi = ring_deq(cstate->complete_obj_ring);
		pthread_mutex_unlock(&cstate->cmutex);

		/* we got through them all */
		if (!oi) break;

		(*lstate->cb.release_obj_cb) (cstate->app_cookie, oi->obj);
		free(oi);
	}

	while (1) {
		pthread_mutex_lock(&cstate->cmutex);
		oi = ring_deq(cstate->partial_obj_ring);
		pthread_mutex_unlock(&cstate->cmutex);

		/* we got through them all */
		if (!oi) return 0;

		(*lstate->cb.release_obj_cb) (cstate->app_cookie, oi->obj);
		free(oi);
	}

	return (0);
}


/*
 * This is the initialization function that is
 * called by the searchlet library when we startup.
 */

/*
 * XXX callback for new packets 
 */
void           *
sstub_init(sstub_cb_args_t * cb_args)
{
	return sstub_init_ext(cb_args, 0, 0);
}


/*
 * This is a new version of sstub_init which allows
 * for binding only to localhost.
 */
void *
sstub_init_2(sstub_cb_args_t * cb_args,
	     int bind_only_locally)
{
	return sstub_init_ext(cb_args, bind_only_locally, 0);
}

/*
 * This is a new version of sstub_init which allows
 * for binding only to localhost.
 */
void *
sstub_init_ext(sstub_cb_args_t * cb_args,
	     int bind_only_locally,
	     int auth_required)
{
	listener_state_t *list_state;
	int             err;

	list_state = (listener_state_t *) calloc(1, sizeof(*list_state));
	if (list_state == NULL) {
		return (NULL);
	}

	/* Save all the callback functions. */
	list_state->cb = *cb_args;

	/*
	 * save authentication state
	 */
	if (auth_required) {
		list_state->flags |= LSTATE_AUTH_REQUIRED;
	}

	/*
	 * Open the listner sockets for the different types of connections.
	 */
	err = sstub_new_sock(&list_state->control_fd, 
 			     diamond_get_control_port(),
			     bind_only_locally);
	if (err) {
		/*
		 * XXX log, 
		 */
		printf("failed to create control \n");
		free(list_state);
		return (NULL);
	}

	err = sstub_new_sock(&list_state->data_fd, 
			     diamond_get_data_port(),
			     bind_only_locally);
	if (err) {
		/*
		 * XXX log, 
		 */
		printf("failed to create data \n");
		free(list_state);
		return (NULL);
	}

	sstub_set_states(NULL, NULL);

	return ((void *) list_state);
}
