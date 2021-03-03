/* -*- C -*- */
/*
 * Copyright (c) 2020 Seagate Technology LLC and/or its Affiliates
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * For any questions about this software or licensing,
 * please email opensource@seagate.com or cortx-questions@seagate.com.
 *
 */

/**
 * @addtogroup netlibfab
 *
 * @{
 */

#define M0_TRACE_SUBSYSTEM M0_TRACE_SUBSYS_NET
#include "lib/trace.h"          /* M0_ENTRY() */
#include <netinet/in.h>         /* INET_ADDRSTRLEN */
#include <arpa/inet.h>          /* inet_pton, htons */
#include <stdlib.h>             /* atoi */
#include "net/net.h"            /* struct m0_net_domain */
#include "lib/memory.h"         /* M0_ALLOC_PTR()*/
#include "libfab_internal.h"
#include "net/net_internal.h"   /* m0_net__buffer_invariant() */

static char     *providers[] = { "verbs", "tcp", "sockets" };
static char     *portf[PORTFAMILYMAX]  = { "unix", "inet", "inet6" };
static char     *socktype[SOCKTYPEMAX] = { "stream", "dgram" };
static char      fab_autotm[1024] = {};
static uint64_t  mr_key_idx = 0;

M0_TL_DESCR_DEFINE(fab_buf, "libfab_buf",
		   static, struct m0_fab__buf, fb_linkage, fb_magic,
		   M0_NET_LIBFAB_BUF_MAGIC, M0_NET_LIBFAB_BUF_HEAD_MAGIC);
M0_TL_DEFINE(fab_buf, static, struct m0_fab__buf);

M0_TL_DESCR_DEFINE(fab_sndbuf, "libfab_sndbuf",
		   static, struct m0_fab__buf, fb_snd_link, fb_sndmagic,
		   M0_NET_LIBFAB_SNDBUF_MAGIC, M0_NET_LIBFAB_SNDBUF_HEAD_MAGIC);
M0_TL_DEFINE(fab_sndbuf, static, struct m0_fab__buf);

M0_TL_DESCR_DEFINE(fab_fabs, "libfab_fabrics",
		   static, struct m0_fab__fab, fab_link, fab_magic,
		   M0_NET_LIBFAB_FAB_MAGIC, M0_NET_LIBFAB_FAB_HEAD_MAGIC);
M0_TL_DEFINE(fab_fabs, static, struct m0_fab__fab);

static int libfab_ep_addr_decode(struct m0_fab__ep *ep, const char *name);
static int libfab_ep_res_init(struct m0_fab__active_ep *aep,
			      struct m0_fab__tm *tm);
static int libfab_pep_res_init(struct m0_fab__passive_ep *pep,
			       struct m0_fab__tm *tm);
static struct m0_fab__ep *libfab_ep_net(struct m0_net_end_point *net);
static bool libfab_ep_cmp(struct m0_fab__ep *ep, const char *name,
			  struct m0_fab__ep_name *epname);
static int libfab_ep_find(struct m0_net_transfer_mc *tm, const char *name,
			  struct m0_fab__ep_name *epn,
			  struct m0_net_end_point **epp);
static int libfab_ep_create(struct m0_net_transfer_mc *tm, const char *name, 
			    struct m0_fab__ep_name *epn,
			    struct m0_net_end_point **epp);
static int libfab_active_ep_create(struct m0_fab__ep *ep, struct m0_fab__tm *tm,
				   struct fi_info *fi);
static int libfab_passive_ep_create(struct m0_fab__ep *ep,
				    struct m0_fab__tm *tm);
static int libfab_aep_param_free(struct m0_fab__active_ep *aep,
				 struct m0_fab__tm *tm);
static int libfab_pep_param_free(struct m0_fab__passive_ep *pep,
				 struct m0_fab__tm *tm);
static int libfab_ep_param_free(struct m0_fab__ep *ep, struct m0_fab__tm *tm);
static int libfab_ep_res_free(struct m0_fab__ep_res *ep_res, 
			      struct m0_fab__tm *tm);
static void libfab_poller(struct m0_fab__tm *ma);
static int libfab_waitset_init(struct m0_fab__tm *tm);
static void libfab_tm_event_post(struct m0_fab__tm *tm, 
				 enum m0_net_tm_state state);
static void libfab_tm_lock(struct m0_fab__tm *tm);
static void libfab_tm_unlock(struct m0_fab__tm *tm);
static void libfab_tm_evpost_lock(struct m0_fab__tm *tm);
static void libfab_tm_evpost_unlock(struct m0_fab__tm *tm);
static bool libfab_tm_is_locked(const struct m0_fab__tm *tm);
static void libfab_buf_complete(struct m0_fab__buf *buf, int32_t status);
static void libfab_buf_done(struct m0_fab__buf *buf, int rc);
static bool libfab_tm_invariant(const struct m0_fab__tm *tm);
static struct m0_fab__tm *libfab_buf_ma(struct m0_net_buffer *buf);
static int libfab_bdesc_encode(struct m0_fab__buf *buf);
static void libfab_bdesc_decode(struct m0_net_buf_desc *nbd, uint64_t *rma_key,
				struct m0_fab__ep_name *epname);
static void libfab_buf_del(struct m0_net_buffer *nb);
static void libfab_ep_put(struct m0_fab__ep *ep);
static void libfab_ep_get(struct m0_fab__ep *ep);
static void libfab_ep_release(struct m0_ref *ref);
static uint64_t libfab_mr_keygen(void);
static int libfab_check_for_event(struct fid_eq *eq);
static int libfab_check_for_comp(struct fid_cq *cq, struct m0_fab__buf **ctx,
				 m0_bindex_t *len);
static void libfab_tm_fini(struct m0_net_transfer_mc *tm);
static int libfab_buf_dom_reg(struct m0_net_buffer *nb, struct fid_domain *dp);
static int libfab_destaddr_get(struct m0_fab__ep_name *epname,
			       struct fi_info *hints, struct fi_info **out);
static void libfab_pending_bufs_send(struct m0_fab__ep *ep);
static int libfab_target_notify(struct m0_fab__buf *buf,
				struct m0_fab__active_ep *ep);
static m0_bcount_t libfab_iov_prep(struct m0_bufvec *bv, struct iovec *iv,
				   int nr, m0_bcount_t nblen,
				   m0_bcount_t *count);
static struct m0_fab__fab *libfab_newfab_init(struct m0_fab__list *fl);
static int libfab_conn_init(struct m0_fab__ep *ep, struct m0_fab__tm *ma,
			    struct m0_fab__buf *fbp, const char *name);
static int libfab_fab_ep_find(struct m0_fab__tm *tm, struct m0_fab__ep_name *en,
			      const char *name, struct m0_fab__ep **ep);
static void libfab_ep_pton(struct m0_fab__ep_name *name, uint64_t *out);
static void libfab_ep_ntop(uint64_t netaddr, struct m0_fab__ep_name *name);

/* libfab init and fini() : initialized in motr init */
M0_INTERNAL int m0_net_libfab_init(void)
{
	int result = 0;

	m0_net_xprt_register(&m0_net_libfab_xprt);
	m0_net_xprt_default_set(&m0_net_libfab_xprt);
	return M0_RC(result);
}

M0_INTERNAL void m0_net_libfab_fini(void)
{
	m0_net_xprt_deregister(&m0_net_libfab_xprt);
}

/**
 * Bitmap of used transfer machine identifiers.
 *
 * This is used to allocate unique transfer machine identifiers for LNet network
 * addresses with wildcard transfer machine identifier (like
 * "192.168.96.128@tcp1:12345:31:*").
 *
 */
static int libfab_ep_addr_decode_lnet(const char *name, char *node,
				      size_t nodeSize, char *port,
				      size_t portSize)
{
	char     *at = strchr(name, '@');
	int       nr;
	unsigned  pid;
	unsigned  portal;
	unsigned  portnum;
	unsigned  tmid;
	char      *lp = "127.0.0.1";

	if (strncmp(name, "0@lo", 4) == 0) {
		M0_PRE(nodeSize >= ((strlen(lp)+1)) );
		memcpy(node, lp, (strlen(lp)+1));
	} else {
		if (at == NULL || at - name >= nodeSize)
			return M0_ERR(-EPROTO);

		M0_PRE(nodeSize >= (at-name)+1);
		memcpy(node, name, at - name);
	}
	if ((at = strchr(at, ':')) == NULL) /* Skip 'tcp...:' bit. */
		return M0_ERR(-EPROTO);
	nr = sscanf(at + 1, "%u:%u:%u", &pid, &portal, &tmid);
	if (nr != 3) {
		nr = sscanf(at + 1, "%u:%u:*", &pid, &portal);
		if (nr != 2)
			return M0_ERR(-EPROTO);
		for (nr = 0; nr < ARRAY_SIZE(fab_autotm); ++nr) {
			if (fab_autotm[nr] == 0) {
				tmid = nr;
				break;
			}
		}
		if (nr == ARRAY_SIZE(fab_autotm))
			return M0_ERR(-EADDRNOTAVAIL);
	}
	/*
	* Hard-code LUSTRE_SRV_LNET_PID to avoid dependencies on the Lustre
	* headers.
	*/
	if (pid != 12345)
		return M0_ERR(-EPROTO);
	/*
	* Deterministically combine portal and tmid into a unique 16-bit port
	* number (greater than 1024). Tricky.
	*
	* Port number is, in binary: tttttttttt1ppppp, that is, 10 bits of tmid
	* (which must be less than 1024), followed by a set bit (guaranteeing
	* that the port is not reserved), followed by 5 bits of (portal - 30),
	* so that portal must be in the range 30..61.
	*
	if (tmid >= 1024 || (portal - 30) >= 32)
		return M0_ERR_INFO(-EPROTO,
			"portal: %u, tmid: %u", portal, tmid);
	*/

	portnum  = tmid | (1 << 10) | ((portal - 30) << 11);
	sprintf(port, "%d", portnum);
	fab_autotm[tmid] = 1;
	return M0_RC(0);
}

static int libfab_ep_addr_decode_sock(const char *ep_name, char *node,
				      size_t nodeSize, char *port,
				      size_t portSize)
{
	int   shift = 0;
	int   f;
	int   s;
	char *at;

	for (f = 0; f < PORTFAMILYMAX ; ++f) {
		if (portf[f]!= NULL) {
			shift = strlen(portf[f]);
			if (strncmp(ep_name, portf[f], shift) == 0)
				break;
		}
	}
	if (ep_name[shift] != ':')
		return M0_ERR(-EINVAL);
	ep_name += shift + 1;
	for (s = 0; s < SOCKTYPEMAX; ++s) {
		if (socktype[s] != NULL) {
			shift = strlen(socktype[s]);
			if (strncmp(ep_name, socktype[s], shift) == 0)
				break;
		}
	}
	if (ep_name[shift] != ':')
		return M0_ERR(-EINVAL);
	ep_name += shift + 1;
	at = strchr(ep_name, '@');
	if (at == NULL) {
		return M0_ERR(-EINVAL);
	} else {
		at++;
		if (at == NULL)
			return M0_ERR(-EINVAL);
		M0_PRE(portSize >= (strlen(at)+1));
		memcpy(port,at,(strlen(at)+1));
	}
	M0_PRE(nodeSize >= (at - ep_name));
	memcpy(node, ep_name, ((at - ep_name)-1));
	return 0;
}

/**
 * Used to take the ip and port from the given end point
 * ep_name : endpoint address from domain
 * node    : copy ip address from ep_name
 * port    : copy port number from ep_name
 * Example of ep_name IPV4 192.168.0.1:4235
 *                    IPV6 [4002:db1::1]:4235
 */
static int libfab_ep_addr_decode_native(const char *ep_name, char *node,
					size_t nodeSize, char *port,
					size_t portSize)
{
	char   *name;
	char   *cp;
	size_t  n;
	int     rc = 0;

	M0_PRE(ep_name != NULL);

	M0_ENTRY("ep_name=%s", ep_name);

	name = (char *)ep_name + strlen("libfab:");

	if( name[0] == '[' ) {
		/* IPV6 pattern */
		cp = strchr(name, ']');
		if (cp == NULL)
			return M0_ERR(-EINVAL);

		name++;
		n = cp - name;
		if (n == 0 )
			return M0_ERR(-EINVAL);
		cp++;
		if (*cp != ':')
		return M0_ERR(-EINVAL);
		cp++;
	}
	else {
		/* IPV4 pattern */
		cp = strchr(name, ':');
		if (cp == NULL)
			return M0_ERR(-EINVAL);

		n = cp - name;
		if (n == 0 )
			return M0_ERR(-EINVAL);

		++cp;
	}

	M0_PRE(nodeSize >= (n+1));
	M0_PRE(portSize >= (strlen(cp)+1));

	memcpy(node, name, n);
	node[n] = 0;

	n=strlen(cp);
	memcpy(port, cp, n);
	port[n] = 0;

	return M0_RC(rc);
}

/**
 * Parses network address.
 *
 * The following address formats are supported:
 *
 *     - lnet compatible, see nlx_core_ep_addr_decode():
 *
 *           nid:pid:portal:tmid
 *
 *       for example: "10.0.2.15@tcp:12345:34:123" or
 *       "192.168.96.128@tcp1:12345:31:*"
 *
 *     - sock format, see socket(2):
 *           family:type:ipaddr[@port]
 *
 *
 *     - libfab compatible format
 *       for example IPV4 libfab:192.168.0.1:4235
 *                   IPV6 libfab:[4002:db1::1]:4235
 *
 */
static int libfab_ep_addr_decode(struct m0_fab__ep *ep, const char *name)
{
	char *node = ep->fep_name.fen_addr;
	char *port = ep->fep_name.fen_port;
	size_t nodeSize = ARRAY_SIZE(ep->fep_name.fen_addr);
	size_t portSize = ARRAY_SIZE(ep->fep_name.fen_port);
	int result;

	if( name == NULL || name[0] == 0)
		result =  M0_ERR(-EPROTO);
	else if((strncmp(name,"libfab",6))==0)
		result = libfab_ep_addr_decode_native(name, node, nodeSize, 
						      port, portSize);
	else if (name[0] < '0' || name[0] > '9')
		/* sock format */
		result = libfab_ep_addr_decode_sock(name, node, nodeSize, 
						    port, portSize);
	else
		/* Lnet format. */
		result = libfab_ep_addr_decode_lnet(name, node, nodeSize, 
						    port, portSize);

	if (result == FI_SUCCESS)
		strcpy(ep->fep_name.fen_str_addr, name);

	return M0_RC(result);
}

static void libfab_tm_lock(struct m0_fab__tm *tm)
{
	m0_mutex_lock(&tm->ftm_net_ma->ntm_mutex);
}

static void libfab_tm_unlock(struct m0_fab__tm *tm)
{
	m0_mutex_unlock(&tm->ftm_net_ma->ntm_mutex);
}

static void libfab_tm_evpost_lock(struct m0_fab__tm *tm)
{
	m0_mutex_lock(&tm->ftm_evpost);
}

static void libfab_tm_evpost_unlock(struct m0_fab__tm *tm)
{
	m0_mutex_unlock(&tm->ftm_evpost);
}

static bool libfab_tm_is_locked(const struct m0_fab__tm *tm)
{
	return m0_mutex_is_locked(&tm->ftm_net_ma->ntm_mutex);
}

/**
 * Helper function that posts a tm state change event.
 */
static void libfab_tm_event_post(struct m0_fab__tm *tm,
				 enum m0_net_tm_state state)
{
	struct m0_net_end_point *listen = NULL;

	if (state == M0_NET_TM_STARTED) {
		/* Check for LISTENING Passive endpoint */
		listen = &tm->ftm_pep->fep_nep;
		M0_ASSERT(listen != NULL);
	}
	
	m0_net_tm_event_post(&(struct m0_net_tm_event) {
		.nte_type       = M0_NET_TEV_STATE_CHANGE,
		.nte_next_state = state,
		.nte_time       = m0_time_now(),
		.nte_ep         = listen,
		.nte_tm         = tm->ftm_net_ma,
	});
}

/**
 * Finds queued buffers that timed out and completes them with a
 * prejudice error.
 */
static void libfab_tm_buf_timeout(struct m0_fab__tm *ftm)
{
	struct m0_net_transfer_mc *net = ftm->ftm_net_ma;
	int                        i;
	m0_time_t                  now = m0_time_now();

	M0_PRE(libfab_tm_invariant(ftm));
	for (i = 0; i < ARRAY_SIZE(net->ntm_q); ++i) {
		struct m0_net_buffer *nb;

		m0_tl_for(m0_net_tm, &ftm->ftm_net_ma->ntm_q[i], nb) {
			if (nb->nb_timeout < now) {
				nb->nb_flags |= M0_NET_BUF_TIMED_OUT;
				libfab_buf_done(nb->nb_xprt_private,
						-ETIMEDOUT);
			}
		} m0_tl_endfor;
	}
	M0_POST(libfab_tm_invariant(ftm));
}

/**
 * Finds buffers pending completion and completes them.
 *
 * A buffer is placed on ma::t_done queue when its operation is done, but the
 * completion call-back cannot be immediately invoked, for example, because
 * completion happened in a synchronous context.
 */
static void libfab_tm_buf_done(struct m0_fab__tm *ftm)
{
	struct m0_fab__buf *buffer;
	int                 nr = 0;

	M0_PRE(libfab_tm_is_locked(ftm) && libfab_tm_invariant(ftm));
	m0_tl_for(fab_buf, &ftm->ftm_done, buffer) {
		fab_buf_tlist_del(buffer);
		libfab_buf_complete(buffer, buffer->fb_status);
		nr++;
	} m0_tl_endfor;

	if (nr > 0 && ftm->ftm_net_ma->ntm_callback_counter == 0)
		m0_chan_broadcast(&ftm->ftm_net_ma->ntm_chan);
	M0_POST(libfab_tm_invariant(ftm));
}

/**
 * Used to monitor connection request events
 */
static uint32_t libfab_handle_connect_request_events(struct m0_fab__tm *tm)
{
	struct m0_fab__ep        *ep = NULL;
	struct m0_fab__conn_data *cd;
	struct m0_fab__ep_name    en;
	struct fid_eq            *eq;
	struct fi_eq_err_entry    eq_err;
	struct fi_eq_cm_entry     entry;
	uint32_t                  event;
	int                       rc;

	eq = tm->ftm_pep->fep_listen->pep_ep_res.fer_eq;
	rc = fi_eq_read(eq, &event, &entry,
			(sizeof(entry) + LIBFAB_ADDR_STRLEN_MAX), 0);
	if (rc >= sizeof(entry)) {
		if (event == FI_CONNREQ) {
			memset(&en, 0, sizeof(en));
			cd = (struct m0_fab__conn_data*)entry.data;
			libfab_ep_ntop(cd->fcd_netaddr, &en);
			libfab_fab_ep_find(tm, &en, cd->fcd_straddr, &ep);
			rc = libfab_active_ep_create(ep, tm, entry.info);
			fi_freeinfo(entry.info);
		}
	} else if (rc == -FI_EAVAIL) {
		memset(&eq_err, 0, sizeof(eq_err));
		rc = fi_eq_readerr(eq, &eq_err, 0);
		if (rc != sizeof(eq_err)) {
			M0_LOG(M0_ERROR, "fi_eq_readerr returns error =%s",
			       fi_strerror((int) -(rc)));
		} else {
			M0_LOG(M0_ERROR, "fi_eq_readerr provider err no %d:%s",
				eq_err.prov_errno,
				fi_eq_strerror(eq, eq_err.prov_errno,
					       eq_err.err_data, NULL, 0));
		}
	}
	return 0;
}

/**
 * Used to poll for connection and completion events
 */
static void libfab_poller(struct m0_fab__tm *tm)
{
	struct m0_net_end_point  *net;
	struct m0_fab__ep        *xep;
	struct m0_fab__buf       *buf;
	struct m0_fab__active_ep *aep;
	struct fid_cq            *cq;
	m0_bindex_t               len = 0;
	int                       rc = 0;

	while (tm->ftm_shutdown == false) {
		while(1) {
			m0_mutex_lock(&tm->ftm_endlock);
			if (tm->ftm_shutdown == true)
				break;
			else if (m0_mutex_trylock(&tm->ftm_net_ma->ntm_mutex) 
									 != 0) {
				m0_mutex_unlock(&tm->ftm_endlock);
			} else
				break;
		}
		
		m0_mutex_unlock(&tm->ftm_endlock);
		
		if (tm->ftm_shutdown == true)
			break;
		
		M0_ASSERT(libfab_tm_is_locked(tm) && libfab_tm_invariant(tm));

		libfab_handle_connect_request_events(tm);

		buf = NULL;
		cq = tm->ftm_tx_cq;
		rc = libfab_check_for_comp(cq, &buf, NULL);
		if (buf != NULL && rc != -FI_EAGAIN) {
			xep = buf->fb_txctx;
			if (rc > 0) {
				aep = (xep->fep_listen == NULL) ? xep->fep_send
						: xep->fep_listen->pep_tx_ep;
				libfab_target_notify(buf, aep);
				libfab_buf_done(buf, 0);
			}
			else
				libfab_buf_done(buf, -ECANCELED);
		}

		m0_tl_for(m0_nep, &tm->ftm_net_ma->ntm_end_points, net) {
			xep = libfab_ep_net(net);
			aep = (xep->fep_listen == NULL) ? xep->fep_send :
			      xep->fep_listen->pep_tx_ep;
			if (aep != NULL) {
				if (aep->aep_state == FAB_CONNECTING) {
					rc = libfab_check_for_event(
					      aep->aep_ep_res.fer_eq);
					if (rc == FI_CONNECTED) {
						libfab_pending_bufs_send(xep);
						aep->aep_state = FAB_CONNECTED;
					}
				}

				buf = NULL;
				len = 0;
				cq = (xep->fep_listen == NULL) ?
						     aep->aep_ep_res.fer_rx_cq :
				xep->fep_listen->pep_rx_ep->aep_ep_res.fer_rx_cq;
				if (cq != NULL) {
					rc = libfab_check_for_comp(cq, &buf, 
								   &len);
					if (buf != NULL) {
						if (rc > 0) {
							if (buf->fb_length == 0)
								buf->fb_length = len;
							buf->fb_ev_ep = xep;
							libfab_ep_get(xep);
							libfab_buf_done(buf, 0);
						}
						else if (rc != -FI_EAGAIN)
							libfab_buf_done(buf,
								    -ECANCELED);
					}
				}
			}
		} m0_tl_endfor;

		libfab_tm_buf_timeout(tm);
		libfab_tm_buf_done(tm);

		M0_ASSERT(libfab_tm_invariant(tm));
		libfab_tm_unlock(tm);
	}
}

/** 
 * Converts generic end-point to its libfabric structure.
 */
static struct m0_fab__ep *libfab_ep_net(struct m0_net_end_point *net)
{
	return container_of(net, struct m0_fab__ep, fep_nep);
}

/**
 * Compares the endpoint name  with the passed name string and
 * returns true if equal, or else returns false
*/
static bool libfab_ep_cmp(struct m0_fab__ep *ep, const char *name,
			  struct m0_fab__ep_name *epname)
{
	bool ret = false;
	if (name == NULL) {
		if ((strcmp(ep->fep_name.fen_addr, epname->fen_addr) == 0) &&
		    (strcmp(ep->fep_name.fen_port, epname->fen_port) == 0))
			ret = true;
	}
	else {
		if (strcmp(ep->fep_name.fen_str_addr, name) == 0)
			ret = true;
	}
	return ret;
}

/**
 * Search for the ep in the existing ep list.
 * If found then return the ep structure, or else create a new endpoint 
 * with the name
 */
static int libfab_ep_find(struct m0_net_transfer_mc *tm, const char *name,
			  struct m0_fab__ep_name *epn,
			  struct m0_net_end_point **epp)
{
	struct m0_net_end_point *net;
	struct m0_fab__ep       *xep;
	char                     ep[LIBFAB_ADDR_STRLEN_MAX] = {0};
	bool                     found = false;
	int                      rc = 0;

	M0_ENTRY();

	m0_tl_for(m0_nep, &tm->ntm_end_points, net) {
		xep = libfab_ep_net(net);
		if (libfab_ep_cmp(xep, name, epn) == true) {
			*epp = &xep->fep_nep;
			found = true;
			break;
		}
	} m0_tl_endfor;

	if (found == false) {
		if (name != NULL)
			rc = libfab_ep_create(tm, name, epn, epp);
		else {
			sprintf(ep, "libfab:%s:%s", epn->fen_addr, epn->fen_port);
			rc = libfab_ep_create(tm, ep, epn, epp);
		}
	}

	return M0_RC(rc);
}

/**
 * Used to create an endpoint
 */
static int libfab_ep_create(struct m0_net_transfer_mc *tm, const char *name,
			    struct m0_fab__ep_name *epn,
			    struct m0_net_end_point **epp)
{
	struct m0_fab__tm *ma = tm->ntm_xprt_private;
	struct m0_fab__ep *ep = NULL;
	char              *wc;
	int                rc;

	M0_ENTRY();
	M0_PRE(name != NULL);

	M0_ALLOC_PTR(ep);
	if (ep == NULL)
		return M0_ERR(-ENOMEM);

	M0_ALLOC_PTR(ep->fep_send);
	if (ep->fep_send == NULL)
		return M0_ERR(-ENOMEM);

	ep->fep_listen = NULL;

	rc = libfab_ep_addr_decode(ep, name);
	if (rc != FI_SUCCESS) {
		libfab_aep_param_free(ep->fep_send, ma);
		return M0_RC(rc);
	}

	wc = strchr(name, '*');
	if (epn != NULL && wc != NULL) {
		strcpy(ep->fep_name.fen_addr, epn->fen_addr);
		strcpy(ep->fep_name.fen_port, epn->fen_port);
	}

	rc = libfab_active_ep_create(ep, ma, NULL);
	if (rc != FI_SUCCESS) {
		return M0_RC(rc);
	}

	fab_sndbuf_tlist_init(&ep->fep_sndbuf);
	*epp = &ep->fep_nep;
	return M0_RC(rc);
}

/**
 * Init resources for a transfer machine
 */
static int libfab_tm_res_init(struct m0_fab__tm *tm)
{
	struct m0_fab__fab        *fab;
	struct m0_fab__passive_ep *pep;
	struct fi_cq_attr          cq_attr;
	int                        rc = 0;
	
	M0_ENTRY();
	
	M0_PRE(tm != NULL);

	pep = tm->ftm_pep->fep_listen;
	fab = tm->ftm_fab;
	memset(&cq_attr, 0, sizeof(cq_attr));
	/* Initialise completion queues for tx */
	cq_attr.wait_obj = FI_WAIT_NONE;
	cq_attr.format = FI_CQ_FORMAT_MSG;
	cq_attr.size = fab->fab_fi->tx_attr->size;
	rc = fi_cq_open(fab->fab_dom, &cq_attr, &tm->ftm_tx_cq, NULL);
	if (rc != FI_SUCCESS)
		return M0_ERR(rc);

	/* Initialize and bind resources to tx ep */
	rc = fi_endpoint(fab->fab_dom, fab->fab_fi, &pep->pep_tx_ep->aep_ep,
			 NULL);
	if (rc != FI_SUCCESS)
		return M0_ERR(rc);
	rc = libfab_ep_res_init(pep->pep_tx_ep, tm);

	return M0_RC(rc);
}

/**
 * Init resources and bind it to the active endpoint.
 */
static int libfab_ep_res_init(struct m0_fab__active_ep *aep,
			      struct m0_fab__tm *tm)
{
	struct fi_cq_attr   cq_attr;
	struct fi_eq_attr   eq_attr;
	struct m0_fab__fab *fab;
	int                 rc = 0;

	M0_ENTRY();
	
	M0_PRE(tm->ftm_waitset != NULL);

	fab = tm->ftm_fab;

	memset(&cq_attr, 0, sizeof(cq_attr));
	/* Initialise and bind completion queues for rx */
	cq_attr.wait_obj = FI_WAIT_NONE;
	cq_attr.format = FI_CQ_FORMAT_MSG;
	cq_attr.size = fab->fab_fi->rx_attr->size;
	rc = fi_cq_open(fab->fab_dom, &cq_attr, &aep->aep_ep_res.fer_rx_cq,
			aep->aep_ep_res.fer_rx_cq);
	if (rc != FI_SUCCESS)
		return M0_RC(rc);
	
	rc = fi_ep_bind(aep->aep_ep, &tm->ftm_tx_cq->fid, FI_TRANSMIT);
	if (rc != FI_SUCCESS)
		return M0_RC(rc);
	rc = fi_ep_bind(aep->aep_ep, &aep->aep_ep_res.fer_rx_cq->fid, FI_RECV);
	if (rc != FI_SUCCESS)
		return M0_RC(rc);

	/* Initialise and bind event queue */
	memset(&eq_attr, 0, sizeof(eq_attr));
	eq_attr.wait_obj = FI_WAIT_UNSPEC;
	rc = fi_eq_open(fab->fab_fab, &eq_attr, &aep->aep_ep_res.fer_eq, NULL);
	if (rc != FI_SUCCESS)
		return M0_RC(rc);

	rc = fi_ep_bind(aep->aep_ep, &aep->aep_ep_res.fer_eq->fid, 0);
	if (rc != FI_SUCCESS)
		return M0_RC(rc);

	/* Bind shared recv context */
	rc = fi_ep_bind(aep->aep_ep, &tm->ftm_rctx->fid, 0);
	if (rc != FI_SUCCESS)
		return M0_RC(rc);

	return M0_RC(rc);
}

/**
 * Init resources and bind it to the passive endpoint.
 */
static int libfab_pep_res_init(struct m0_fab__passive_ep *pep,
			       struct m0_fab__tm *tm)
{
	struct fi_eq_attr eq_attr;
	int               rc = 0;
	
	M0_ENTRY();

	M0_PRE(tm->ftm_waitset != NULL);

	/* Initialise and bind event queue */
	memset(&eq_attr, 0, sizeof(eq_attr));
	eq_attr.wait_obj = FI_WAIT_UNSPEC;
	rc = fi_eq_open(tm->ftm_fab->fab_fab, &eq_attr, &pep->pep_ep_res.fer_eq,
			NULL);
	if (rc != FI_SUCCESS)
		return M0_RC(rc);
	
	rc = fi_pep_bind(pep->pep_pep, &pep->pep_ep_res.fer_eq->fid, 0);
	if (rc != FI_SUCCESS)
		return M0_RC(rc);

	pep->pep_ep_res.fer_rx_cq = NULL;

	return M0_RC(rc);
}

/**
 * Used to create an active endpoint
 */
static int libfab_active_ep_create(struct m0_fab__ep *ep, struct m0_fab__tm *tm,
				   struct fi_info *info)
{
	struct m0_net_end_point  *net;
	struct m0_fab__active_ep *aep;
	struct fi_info           *fi;
	struct fid_domain        *dp;
	int                       rc;

	M0_ENTRY();

	aep = (ep->fep_listen != NULL) ? ep->fep_listen->pep_rx_ep :
					 ep->fep_send;
	fi = (info == NULL) ? tm->ftm_fab->fab_fi : info;
	dp = tm->ftm_fab->fab_dom;
	
	if (info != NULL && aep->aep_ep != NULL) {
		rc = fi_close(&aep->aep_ep->fid);
		if (rc != FI_SUCCESS)
			M0_LOG(M0_ERROR, "ep close = %d",rc);
		libfab_ep_res_free(&aep->aep_ep_res, tm);
	}

	rc = fi_endpoint(dp, fi, &aep->aep_ep, NULL);
	if (rc != FI_SUCCESS) {
		libfab_aep_param_free(aep, tm);
		return M0_RC(rc);
	}

	rc = libfab_ep_res_init(aep, tm);
	if (rc != FI_SUCCESS) {
		libfab_aep_param_free(aep, tm);
		return M0_RC(rc);
	}

	rc = fi_enable(aep->aep_ep);
	if (rc != FI_SUCCESS) {
		libfab_aep_param_free(aep, tm);
		return M0_RC(rc);
	}

	if (info != NULL) {
		/* Accept incoming request */
		rc = fi_accept(aep->aep_ep, NULL, 0);
		if (rc != FI_SUCCESS) {
			libfab_aep_param_free(aep, tm);
			return M0_RC(rc);
		}

		while (libfab_check_for_event(aep->aep_ep_res.fer_eq)
							       != FI_CONNECTED);
		aep->aep_state = FAB_CONNECTED;
	} else {
		net = &ep->fep_nep;
		net->nep_tm = tm->ftm_net_ma;
		m0_nep_tlink_init_at_tail(net, &tm->ftm_net_ma->ntm_end_points);
		net->nep_addr = (const char *)(&ep->fep_name.fen_str_addr);
		m0_ref_init(&ep->fep_nep.nep_ref, 1, &libfab_ep_release);
	}

	return M0_RC(rc);
}

/**
 * Used to create a passive endpoint which will listen for incoming connection
 * requests
 */
static int libfab_passive_ep_create(struct m0_fab__ep *ep, 
				    struct m0_fab__tm *tm)
{
	struct m0_fab__passive_ep *pep;
	struct fi_info            *hints;
	struct fi_info            *fi;
	int                        i;
	int                        rc;
	char                      *port = NULL;

	M0_ENTRY();

	M0_ALLOC_PTR(ep->fep_listen);
	if (ep->fep_listen == NULL)
		return M0_RC(-ENOMEM);
	M0_ALLOC_PTR(ep->fep_listen->pep_tx_ep);
	if (ep->fep_listen->pep_tx_ep == NULL)
		return M0_RC(-ENOMEM);
	M0_ALLOC_PTR(ep->fep_listen->pep_rx_ep);
	if (ep->fep_listen->pep_rx_ep == NULL)
		return M0_RC(-ENOMEM);

	pep = ep->fep_listen;

	if (strlen(ep->fep_name.fen_port) != 0){
		port = ep->fep_name.fen_port;
	}

	hints = fi_allocinfo();
	if (hints == NULL)
		return M0_ERR(-ENOMEM);

	hints->ep_attr->type = FI_EP_MSG;
	hints->caps = FI_MSG | FI_RMA;

	for (i = 0; i < ARRAY_SIZE(providers); i++) {
		hints->fabric_attr->prov_name = providers[i];
		rc = fi_getinfo(LIBFAB_VERSION, NULL, port, FI_SOURCE, hints,
				&fi);
		if (rc == FI_SUCCESS)
			break;
	}

	M0_ASSERT(i < ARRAY_SIZE(providers));
	hints->fabric_attr->prov_name = NULL;
	tm->ftm_fab->fab_fi = fi;
	fi_freeinfo(hints);
	
	rc = fi_fabric(tm->ftm_fab->fab_fi->fabric_attr, &tm->ftm_fab->fab_fab,
		       NULL);
	if (rc != FI_SUCCESS) {
		libfab_pep_param_free(pep, tm);
		return M0_RC(rc);
	}

	rc = libfab_waitset_init(tm);
	if (rc != FI_SUCCESS) {
		libfab_pep_param_free(pep, tm);
		return M0_RC(rc);
	}

	rc = fi_passive_ep(tm->ftm_fab->fab_fab, tm->ftm_fab->fab_fi,
			   &pep->pep_pep, NULL);
	if (rc != FI_SUCCESS) {
		libfab_pep_param_free(pep, tm);
		return M0_RC(rc);
	}

	rc = libfab_pep_res_init(pep, tm);
	if (rc != FI_SUCCESS) {
		libfab_pep_param_free(pep, tm);
		return M0_RC(rc);
	}

	rc = fi_listen(pep->pep_pep);
	if (rc != FI_SUCCESS) {
		libfab_pep_param_free(pep, tm);
		return M0_RC(rc);
	}

	rc = fi_domain(tm->ftm_fab->fab_fab, tm->ftm_fab->fab_fi,
		       &tm->ftm_fab->fab_dom, NULL);
	if (rc != FI_SUCCESS) {
		M0_LOG(M0_ERROR," \n fi_domain = %d \n ", rc);
		libfab_pep_param_free(pep, tm);
		return M0_RC(rc);
	}

	rc = fi_srx_context(tm->ftm_fab->fab_dom, tm->ftm_fab->fab_fi->rx_attr,
			    &tm->ftm_rctx, NULL);
	if (rc != FI_SUCCESS) {
		M0_LOG(M0_ERROR," \n fi_srx_context = %d \n ", rc);
		libfab_pep_param_free(pep, tm);
		return M0_RC(rc);
	}

	rc = libfab_tm_res_init(tm);
	fab_sndbuf_tlist_init(&ep->fep_sndbuf);

	return M0_RC(rc);
}

/**
 * Used to free the resources attached to an active ep
 */
static int libfab_ep_res_free(struct m0_fab__ep_res *ep_res,
			      struct m0_fab__tm *tm)
{
	int rc = 0;
	
	M0_ENTRY();

	if (ep_res->fer_eq != NULL){
		rc = fi_close(&(ep_res->fer_eq)->fid);
		if (rc != FI_SUCCESS)
			M0_LOG(M0_ERROR, "fer_eq fi_close ret=%d fid=%d",
			       rc, (int)(ep_res->fer_eq)->fid.fclass);
		ep_res->fer_eq = NULL;
	}

	if (ep_res->fer_rx_cq != NULL){
		rc = fi_close(&(ep_res->fer_rx_cq)->fid);
		if (rc != FI_SUCCESS)
			M0_LOG(M0_ERROR, "fer_rx_cq fi_close ret=%d fid=%d",
			       rc, (int)(ep_res->fer_rx_cq)->fid.fclass);
		ep_res->fer_rx_cq = NULL;
	}

	return M0_RC(rc);
}

/**
 * Used to free the active ep
 */
static int libfab_aep_param_free(struct m0_fab__active_ep *aep,
				 struct m0_fab__tm *tm)
{
	int rc = 0;

	M0_ENTRY();

	if (aep == NULL)
		return M0_RC(0);
	if (aep->aep_ep != NULL) {
		rc = fi_close(&(aep->aep_ep)->fid);
		if (rc != FI_SUCCESS)
			M0_LOG(M0_ERROR, "fep_ep fi_close ret=%d fid=%d",
			       rc, (int)(aep->aep_ep)->fid.fclass);
		aep->aep_ep = NULL;
	}

	rc = libfab_ep_res_free(&aep->aep_ep_res, tm);
	
	m0_free(aep);
	aep = NULL;

	return M0_RC(rc);
}

/**
 * Used to free the passive ep
 */
static int libfab_pep_param_free(struct m0_fab__passive_ep *pep,
				 struct m0_fab__tm *tm)
{
	int rc = 0;

	M0_ENTRY();

	if (pep == NULL)
		return M0_RC(0);
	
	if (pep->pep_pep != NULL) {
		rc = fi_close(&(pep->pep_pep)->fid);
		if (rc != FI_SUCCESS)
			M0_LOG(M0_ERROR, "fep_pep fi_close ret=%d fid=%d",
			       rc, (int)(pep->pep_pep)->fid.fclass);
		pep->pep_pep = NULL;
	}
	
	rc = libfab_aep_param_free(pep->pep_tx_ep, tm);
	rc = libfab_aep_param_free(pep->pep_rx_ep, tm);

	rc = libfab_ep_res_free(&pep->pep_ep_res, tm);
	
	m0_free(pep);
	pep = NULL;

	return M0_RC(rc);
}

static int libfab_ep_param_free(struct m0_fab__ep *ep, struct m0_fab__tm *tm)
{
	int rc = 0;

	if (ep == NULL)
		return M0_RC(0);

	rc = libfab_pep_param_free(ep->fep_listen, tm);
	if (rc != FI_SUCCESS)
		return M0_RC(rc);

	rc = libfab_aep_param_free(ep->fep_send, tm);
	if (rc != FI_SUCCESS)
		return M0_RC(rc);

	memset(&ep->fep_name, 0, sizeof(ep->fep_name));

	return M0_RC(rc);
}

/**
 * Used to free the transfer machine params
 */
static int libfab_tm_param_free(struct m0_fab__tm *tm)
{
	struct m0_net_end_point *net;
	struct m0_fab__ep       *xep;
	int                      rc = 0;

	M0_ENTRY();

	if (tm == NULL)
		return M0_RC(0);

	if (tm->ftm_poller.t_func != NULL) {
		m0_thread_join(&tm->ftm_poller);
		m0_thread_fini(&tm->ftm_poller);
	}

	m0_tl_for(m0_nep, &tm->ftm_net_ma->ntm_end_points, net) {
		xep = libfab_ep_net(net);
		m0_nep_tlist_del(net);
		rc = libfab_ep_param_free(xep, tm);
	} m0_tl_endfor;
	M0_ASSERT(m0_nep_tlist_is_empty(&tm->ftm_net_ma->ntm_end_points));
	tm->ftm_net_ma->ntm_ep = NULL;
	
	if (tm->ftm_rctx != NULL) {
		rc = fi_close(&(tm->ftm_rctx)->fid);
		if (rc != FI_SUCCESS)
			M0_LOG(M0_ERROR, "ftm_rctx fi_close ret=%d fid=%d",
			       rc, (int)(tm->ftm_rctx)->fid.fclass);
		tm->ftm_rctx = NULL;
	}

	if (tm->ftm_tx_cq != NULL){
		rc = fi_close(&(tm->ftm_tx_cq)->fid);
		if (rc != FI_SUCCESS)
			M0_LOG(M0_ERROR, "tx_cq fi_close ret=%d fid=%d",
			       rc, (int)(tm->ftm_tx_cq)->fid.fclass);
		tm->ftm_tx_cq = NULL;
	}
	
	if (tm->ftm_waitset != NULL) {
		rc = fi_close(&(tm->ftm_waitset)->fid);
		if (rc != FI_SUCCESS)
			M0_LOG(M0_ERROR, "ftm_waitset fi_close ret=%d fid=%d",
			       rc, (int)(tm->ftm_waitset)->fid.fclass);
		tm->ftm_waitset = NULL;
	}

	return M0_RC(rc);
}

/**
 * Used to init the waitset for the transfer machine 
 */
static int libfab_waitset_init(struct m0_fab__tm *tm)
{
	struct fi_wait_attr wait_attr;
	int                 rc = 0;

	M0_ENTRY();
	if (tm->ftm_waitset != NULL)
		return M0_RC(rc);

	memset(&wait_attr, 0, sizeof(wait_attr));
	wait_attr.wait_obj = FI_WAIT_UNSPEC;
	rc = fi_wait_open(tm->ftm_fab->fab_fab, &wait_attr, &tm->ftm_waitset);

	return M0_RC(rc);
}

static struct m0_fab__tm *libfab_buf_tm(struct m0_fab__buf *buf)
{
	return buf->fb_nb->nb_tm->ntm_xprt_private;
}

static void libfab_buf_fini(struct m0_fab__buf *buf)
{
	fab_buf_tlink_fini(buf);
	if (buf->fb_ev_ep != NULL) {
		libfab_ep_put(buf->fb_ev_ep);
		buf->fb_ev_ep = NULL;
	}
	buf->fb_length = 0;
}

static bool libfab_dom_invariant(const struct m0_net_domain *dom)
{
	struct m0_fab__list *fl = dom->nd_xprt_private;
	return _0C(!fab_fabs_tlist_is_empty(&fl->fl_head)) &&
	       _0C(dom->nd_xprt == &m0_net_libfab_xprt);
}

static bool libfab_tm_invariant(const struct m0_fab__tm *fab_tm)
{
	return fab_tm != NULL &&
	       fab_tm->ftm_net_ma->ntm_xprt_private == fab_tm &&
	       libfab_dom_invariant(fab_tm->ftm_net_ma->ntm_dom);
}

static bool libfab_buf_invariant(const struct m0_fab__buf *buf)
{
	const struct m0_net_buffer *nb = buf->fb_nb;

	return  (nb->nb_flags == M0_NET_BUF_REGISTERED &&
		 nb->nb_tm == NULL) ^ /* or (exclusively) ... */
		/* it is queued to a machine. */
		(_0C(nb->nb_flags & (M0_NET_BUF_REGISTERED|M0_NET_BUF_QUEUED))&&
		 _0C(nb->nb_tm != NULL) &&
		 _0C(m0_net__buffer_invariant(nb)));
}

/** Invokes completion call-back (releasing tm lock). */
static void libfab_buf_complete(struct m0_fab__buf *buf, int32_t status)
{
	struct m0_fab__tm *ma  = libfab_buf_tm(buf);
	struct m0_net_buffer *nb = buf->fb_nb;
	struct m0_net_buffer_event ev = {
		.nbe_buffer = nb,
		.nbe_status = status,
		.nbe_time   = m0_time_now()
	};

	if (M0_IN(nb->nb_qtype, (M0_NET_QT_MSG_RECV,
				 M0_NET_QT_PASSIVE_BULK_RECV,
				 M0_NET_QT_ACTIVE_BULK_RECV))) {
		ev.nbe_length = buf->fb_length;
	}
	
	if (nb->nb_qtype == M0_NET_QT_MSG_RECV) {
		if (ev.nbe_status == 0 && buf->fb_ev_ep != NULL) {
			ev.nbe_ep = &buf->fb_ev_ep->fep_nep;
			libfab_ep_get(buf->fb_ev_ep);
		}
	}
	ma->ftm_net_ma->ntm_callback_counter++;

	libfab_buf_fini(buf);
	M0_ASSERT(libfab_tm_invariant(ma));
	libfab_tm_evpost_lock(ma);
	libfab_tm_unlock(ma);
	m0_net_buffer_event_post(&ev);
	libfab_tm_lock(ma);
	libfab_tm_evpost_unlock(ma);
	M0_ASSERT(libfab_tm_invariant(ma));
	M0_ASSERT(M0_IN(ma->ftm_net_ma->ntm_state, (M0_NET_TM_STARTED,
					      M0_NET_TM_STOPPING)));
	ma->ftm_net_ma->ntm_callback_counter--;
}

/** Completes the buffer operation. */
static void libfab_buf_done(struct m0_fab__buf *buf, int rc)
{
	struct m0_fab__tm *ma = libfab_buf_tm(buf);

	M0_PRE(libfab_tm_is_locked(ma));
	/*
	 * Multiple libfab_buf_done() calls on the same buffer are possible if
	 * the buffer is cancelled.
	 */
	if (!fab_buf_tlink_is_in(buf)) {
		/* Try to finalise. */
		if (m0_thread_self() == &ma->ftm_poller)
			libfab_buf_complete(buf, rc);
		else {
			/* Otherwise, postpone finalisation to
			* libfab_tm_buf_done(). */
			buf->fb_status = rc;
			fab_buf_tlist_add_tail(&ma->ftm_done, buf);
		}
	}
}

static void libfab_ep_put(struct m0_fab__ep *ep)
{
	m0_ref_put(&ep->fep_nep.nep_ref);
}

static void libfab_ep_get(struct m0_fab__ep *ep)
{
	m0_ref_get(&ep->fep_nep.nep_ref);
}

/**
 * End-point finalisation call-back.
 *
 * Used as m0_net_end_point::nep_ref::release(). This call-back is called when
 * end-point reference count drops to 0.
 */
static void libfab_ep_release(struct m0_ref *ref)
{
	struct m0_net_end_point *nep;
	struct m0_fab__ep       *ep;
	struct m0_fab__tm       *tm;

	nep = container_of(ref, struct m0_net_end_point, nep_ref);
	ep = libfab_ep_net(nep);
	tm = nep->nep_tm->ntm_xprt_private;

	m0_nep_tlist_del(nep);
	libfab_ep_param_free(ep, tm);
	
}

static uint64_t libfab_mr_keygen(void)
{
	uint64_t key = FAB_MR_KEY + mr_key_idx;
	mr_key_idx++;
	return key;
}

static int libfab_check_for_event(struct fid_eq *eq)
{
	struct fi_eq_cm_entry entry;
	uint32_t              event = 0;
	ssize_t               rd;

	rd = fi_eq_read(eq, &event, &entry, sizeof(entry), 0);
	if (rd == -FI_EAVAIL) {
		struct fi_eq_err_entry err_entry;
		fi_eq_readerr(eq, &err_entry, 0);
		M0_LOG(M0_ERROR, "%s %s\n", fi_strerror(err_entry.err),
			fi_eq_strerror(eq, err_entry.prov_errno,
			err_entry.err_data,NULL, 0));
		return rd;
	}

	return event;
}

static int libfab_check_for_comp(struct fid_cq *cq, struct m0_fab__buf **ctx,
				 m0_bindex_t *len)
{
	struct fi_cq_msg_entry entry;
	struct fi_cq_err_entry err_entry;
	int                    ret;
	
	ret = fi_cq_read(cq, &entry, 1);
	if (ret > 0) {
		*ctx = (struct m0_fab__buf *)entry.op_context;
		if (len != NULL)
			*len = entry.len;
	}
	else if (ret != -FI_EAGAIN) {
		fi_cq_readerr(cq, &err_entry, 0);
		M0_LOG(M0_ERROR, "%s %s\n", fi_strerror(err_entry.err),
			fi_cq_strerror(cq, err_entry.prov_errno,
			err_entry.err_data,NULL, 0));
	}

	return ret;
}

static void libfab_tm_fini(struct m0_net_transfer_mc *tm)
{
	struct m0_fab__tm *ma = tm->ntm_xprt_private;
	int                rc = 0;

	M0_ENTRY();

	if (!ma->ftm_shutdown) {
		while(1) {
			libfab_tm_lock(ma);
			if (m0_mutex_trylock(&ma->ftm_evpost) != 0) {
				libfab_tm_unlock(ma);
			} else
				break;
		}
		m0_mutex_unlock(&ma->ftm_evpost);
		m0_mutex_lock(&ma->ftm_endlock);
		ma->ftm_shutdown = true;
		m0_mutex_unlock(&ma->ftm_endlock);

		libfab_tm_buf_done(ma);

		rc = libfab_tm_param_free(ma);
		if (rc != FI_SUCCESS)
			M0_LOG(M0_ERROR, "libfab_tm_param_free ret=%d", rc);

		m0_mutex_fini(&ma->ftm_endlock);
		m0_mutex_fini(&ma->ftm_evpost);
		libfab_tm_unlock(ma);
	}
	
	M0_LEAVE();
}

/** Creates the descriptor for a (passive) network buffer. */
static int libfab_bdesc_encode(struct m0_fab__buf *buf)
{
	struct m0_net_buf_desc *nbd = &buf->fb_nb->nb_desc;
	struct m0_net_buffer   *nb = buf->fb_nb;
	int                     seg_nr = nb->nb_buffer.ov_vec.v_nr;
	struct m0_fab__tm      *tm = libfab_buf_ma(nb);
	uint64_t               *ptr;
	uint64_t                netaddr;

	M0_PRE(seg_nr <= FAB_MR_IOV_MAX);

	nbd->nbd_len = (sizeof(uint64_t) * 2);
	nbd->nbd_data = m0_alloc(nbd->nbd_len);
	if (nbd->nbd_data == NULL)
		return M0_RC(-ENOMEM);

	/* TODO: Use inet_pton */
	ptr = (uint64_t *)nbd->nbd_data;
	libfab_ep_pton(&tm->ftm_pep->fep_name, &netaddr);
	*ptr = netaddr;
	ptr++;
	*ptr = buf->fb_mr.bm_key[0];

	return M0_RC(0);
}

static void libfab_bdesc_decode(struct m0_net_buf_desc *nbd, uint64_t *rma_key,
				struct m0_fab__ep_name *epname)
{
	uint64_t  netaddr;
	uint64_t *ptr = (uint64_t *)nbd->nbd_data;

	netaddr = *ptr;
	ptr++;
	libfab_ep_ntop(netaddr, epname);
	*rma_key = *ptr;
}

/* Register the buffer with the appropriate access for the domain of the ep*/
static int libfab_buf_dom_reg(struct m0_net_buffer *nb, struct fid_domain *dp)
{
	struct m0_fab__buf    *fbp = nb->nb_xprt_private;
	struct m0_fab__buf_mr *mr = &fbp->fb_mr;
	int                    seg_nr = nb->nb_buffer.ov_vec.v_nr;
	int                    i;
	int                    ret = FI_SUCCESS;

	M0_PRE(fbp != NULL && dp != NULL);
	M0_PRE(seg_nr <= FAB_MR_IOV_MAX);

	if (fbp->fb_dp == dp)
		return M0_RC(ret);

	for (i = 0; i < seg_nr; i++) {
		mr->bm_key[i] = libfab_mr_keygen();
		
		ret = fi_mr_reg(dp, nb->nb_buffer.ov_buf[i], 
				nb->nb_buffer.ov_vec.v_count[i],
				FAB_MR_ACCESS, FAB_MR_OFFSET, mr->bm_key[i],
				FAB_MR_FLAG, &mr->bm_mr[i], NULL);

		if (ret != FI_SUCCESS) {
			M0_LOG(M0_ERROR, "\n fi_mr_reg = %d \n",ret);
			return M0_ERR(ret);
		}

		mr->bm_desc[i] = fi_mr_desc(mr->bm_mr[i]);
	}

	fbp->fb_dp = dp;

	return M0_RC(ret);
}

static int libfab_destaddr_get(struct m0_fab__ep_name *epname,
			       struct fi_info *hints, struct fi_info **out)
{
	int ret;

	ret = fi_getinfo(LIBFAB_VERSION, epname->fen_addr, epname->fen_port, 0,
			 hints, out);

	return M0_RC(ret);
}


static void libfab_pending_bufs_send(struct m0_fab__ep *ep)
{
	struct m0_fab__active_ep *aep;
	struct m0_fab__buf       *fbp;
	struct m0_net_buffer     *nb;
	struct iovec              iv[FAB_MR_IOV_MAX];
	m0_bcount_t               cnt = 0;
	int                       iv_cnt;

	aep = (ep->fep_listen == NULL) ? ep->fep_send : 
					 ep->fep_listen->pep_tx_ep;
	m0_tl_for(fab_sndbuf, &ep->fep_sndbuf, fbp) {
		cnt = 0;
		nb = fbp->fb_nb;
		fbp->fb_txctx = ep;
		iv_cnt = libfab_iov_prep(&nb->nb_buffer, iv, ARRAY_SIZE(iv),
					 nb->nb_length, &cnt);
		switch (nb->nb_qtype) {
			case M0_NET_QT_MSG_SEND:
				fi_sendv(aep->aep_ep, iv, fbp->fb_mr.bm_desc,
					 iv_cnt, 0, fbp);
				break;
			case M0_NET_QT_ACTIVE_BULK_RECV:
				fi_readv(aep->aep_ep, iv, fbp->fb_mr.bm_desc,
					 iv_cnt, 0, 0, fbp->fb_rem_key, fbp);
				break;
			case M0_NET_QT_ACTIVE_BULK_SEND:
				fi_writev(aep->aep_ep, iv,fbp->fb_mr.bm_desc,
					 iv_cnt, 0, 0, fbp->fb_rem_key, fbp);
				break;
			default: M0_LOG(M0_ERROR, "Invalid qtype=%d",
					nb->nb_qtype);
				break;
		}
		fab_sndbuf_tlist_del(fbp);
	} m0_tl_endfor;
}

static int libfab_target_notify(struct m0_fab__buf *buf,
				struct m0_fab__active_ep *ep)
{
	uint64_t dummy = 0x1234; /* Dummy data */
	int      ret = 0;
	
	if (buf->fb_nb->nb_qtype == M0_NET_QT_ACTIVE_BULK_RECV ||
	    buf->fb_nb->nb_qtype == M0_NET_QT_ACTIVE_BULK_SEND) {
		ret = fi_send(ep->aep_ep, &dummy, sizeof(buf->fb_rc_buf), NULL,
			      0, NULL);
	}

	return M0_RC(ret);
}

/**
 * Prepares iovec.
 * Fills a supplied iovec "iv", with "nr" elements, to launch vectorised io.
 * Returns the number of elements filled. Returns in *count the total number of
 * bytes to be ioed.
 * "bv" is a data buffer for payload.
 */
static m0_bcount_t libfab_iov_prep(struct m0_bufvec *bv, struct iovec *iv,
				   int nr, m0_bcount_t nblen,
				   m0_bcount_t *count)
{
	struct m0_bufvec_cursor cur;
	m0_bcount_t             len;
	int                     idx;

	M0_PRE(nr > 0);
	len = nblen ? nblen : (M0_BCOUNT_MAX / 2);
	
	m0_bufvec_cursor_init(&cur, bv);
	for (idx = 0; idx < nr && !m0_bufvec_cursor_move(&cur, 0); ++idx) {
		m0_bcount_t frag = m0_bufvec_cursor_step(&cur);

		frag = min64u(frag, len - *count);
		if (frag == 0)
			break;
		iv[idx].iov_base = m0_bufvec_cursor_addr(&cur);
		*count += (iv[idx].iov_len = frag);
		m0_bufvec_cursor_move(&cur, frag);
	}
	M0_POST(m0_reduce(i, idx, 0ULL, + iv[i].iov_len) == *count);
	M0_POST(idx > 0); /* Check that there is some progress. */
	return idx;
}

static struct m0_fab__fab *libfab_newfab_init(struct m0_fab__list *fl)
{
	struct m0_fab__fab *fab = NULL;

	M0_ALLOC_PTR(fab);
	if (fab != NULL) {
		fab_fabs_tlink_init_at_tail(fab, &fl->fl_head);
	}

	return fab;
}

static int libfab_conn_init(struct m0_fab__ep *ep, struct m0_fab__tm *ma,
			    struct m0_fab__buf *fbp, const char *name)
{
	struct m0_fab__active_ep *aep;
	struct fi_info           *peer_fi;
	struct m0_fab__conn_data  cd;
	int                       ret = 0;

	aep = (ep->fep_listen != NULL) ? ep->fep_listen->pep_tx_ep :
						 ep->fep_send;
	if (aep->aep_state == FAB_NOT_CONNECTED) {
		ret = libfab_destaddr_get(&ep->fep_name, ma->ftm_fab->fab_fi,
					  &peer_fi);
		strcpy(cd.fcd_straddr, ma->ftm_pep->fep_name.fen_str_addr);
		libfab_ep_pton(&ma->ftm_pep->fep_name, &cd.fcd_netaddr);
		ret = fi_connect(aep->aep_ep, peer_fi->dest_addr, &cd,
				 sizeof(cd));
		if (ret == FI_SUCCESS)
			aep->aep_state = FAB_CONNECTING;
		else
			M0_LOG(M0_ALWAYS, " Conn req failed dst=%"PRIx64,
				*(uint64_t*)peer_fi->dest_addr);
		fi_freeinfo(peer_fi);
	}
	fab_sndbuf_tlink_init_at_tail(fbp, &ep->fep_sndbuf);

	return ret;
}

static int libfab_fab_ep_find(struct m0_fab__tm *tm, struct m0_fab__ep_name *en,
			      const char *name, struct m0_fab__ep **ep)
{
	struct m0_net_transfer_mc *ntm = tm->ftm_net_ma;
	struct m0_net_end_point   *nep;
	int                        ret;

	ret = libfab_ep_find(ntm, name, en, &nep);
	if (ret == FI_SUCCESS)
		*ep = libfab_ep_net(nep);

	return M0_RC(0);
}

static void libfab_ep_pton(struct m0_fab__ep_name *name, uint64_t *out)
{
	uint32_t addr = 0;
	uint32_t port = 0;

	inet_pton(AF_INET, name->fen_addr, &addr);
	port = htonl(atoi(name->fen_port));

	*out = ((uint64_t)addr << 32) | port;
}

static void libfab_ep_ntop(uint64_t netaddr, struct m0_fab__ep_name *name)
{
	union adpo {
		uint32_t ap[2];
		uint64_t net_addr;
	} ap;
	ap.net_addr = netaddr;
	inet_ntop(AF_INET, &ap.ap[1], name->fen_addr, LIBFAB_ADDR_LEN_MAX);
	ap.ap[0] = ntohl(ap.ap[0]);
	sprintf(name->fen_port, "%d", ap.ap[0]);
}

/*============================================================================*/

/** 
 * Used as m0_net_xprt_ops::xo_dom_init(). 
 */
static int libfab_dom_init(const struct m0_net_xprt *xprt,
			   struct m0_net_domain *dom)
{
	struct m0_fab__list *fab_list;

	M0_ENTRY();

	M0_ALLOC_PTR(fab_list);
	if (fab_list == NULL)
		return M0_ERR(-ENOMEM);


	dom->nd_xprt_private = fab_list;
	fab_fabs_tlist_init(&fab_list->fl_head);

	return M0_RC(0);
}

/** 
 * Used as m0_net_xprt_ops::xo_dom_fini(). 
 */
static void libfab_dom_fini(struct m0_net_domain *dom)
{
	struct m0_fab__list *fl;
	struct m0_fab__fab  *fab;
	int                  rc;
	
	M0_ENTRY();
	libfab_dom_invariant(dom);
	fl = dom->nd_xprt_private;
	m0_tl_for(fab_fabs, &fl->fl_head, fab) {
		if (fab->fab_dom != NULL) {
			rc = fi_close(&(fab->fab_dom)->fid);
			if (rc != FI_SUCCESS)
				M0_LOG(M0_ERROR, "fab_dom fi_close ret=%d", rc);
			fab->fab_dom = NULL;
		}
		
		if (fab->fab_fab != NULL) {
			rc = fi_close(&(fab->fab_fab)->fid);
			if (rc != FI_SUCCESS)
				M0_LOG(M0_ERROR, "fab_fabric fi_close ret=%d",
				       rc);
			fab->fab_fab = NULL;
		}

		if (fab->fab_fi != NULL) {
			fi_freeinfo(fab->fab_fi);
			fab->fab_fi = NULL;
		}

		fab_fabs_tlist_del(fab);
		m0_free(fab);
	} m0_tl_endfor;

	M0_ASSERT(fab_fabs_tlist_is_empty(&fl->fl_head));
	m0_free(fl);
	dom->nd_xprt_private = NULL;

	M0_LEAVE();
}

/**
 * Used as m0_net_xprt_ops::xo_ma_fini().
 */
static void libfab_ma_fini(struct m0_net_transfer_mc *tm)
{
	struct m0_fab__tm *ma = tm->ntm_xprt_private;
	
	M0_ENTRY();
	libfab_tm_fini(tm);
	
	tm->ntm_xprt_private = NULL;
	m0_free(ma);

	M0_LEAVE();
}

/**
 * Initialises transport-specific part of the transfer machine.
 *
 * Used as m0_net_xprt_ops::xo_tm_init().
 */
static int libfab_ma_init(struct m0_net_transfer_mc *ntm)
{
	struct m0_fab__tm *ftm;
	int                rc = 0;

	M0_ASSERT(ntm->ntm_xprt_private == NULL);
	M0_ALLOC_PTR(ftm);
	if (ftm != NULL) {
		ftm->ftm_shutdown = false;
		ntm->ntm_xprt_private = ftm;
		ftm->ftm_net_ma = ntm;
		fab_buf_tlist_init(&ftm->ftm_done);
	} else
		rc = M0_ERR(-ENOMEM);

	if (rc != 0)
		libfab_ma_fini(ntm);
	return M0_RC(rc);
}

/**
 * Starts initialised ma.
 *
 * Initialises everything that libfab_ma_init() didn't. Note that ma is in
 * M0_NET_TM_STARTING state after this returns. Switch to M0_NET_TM_STARTED
 * happens when the poller thread posts special event.
 *
 * Used as m0_net_xprt_ops::xo_tm_start().
 */
static int libfab_ma_start(struct m0_net_transfer_mc *ntm, const char *name)
{
	struct m0_fab__tm       *ftm = ntm->ntm_xprt_private;
	struct m0_fab__list     *fl;
	struct m0_net_end_point *nep;
	int                      rc = 0;

	M0_ALLOC_PTR(ftm->ftm_pep);
	if (ftm->ftm_pep != NULL)
		libfab_ep_addr_decode(ftm->ftm_pep, name);
		
		fl = ntm->ntm_dom->nd_xprt_private;
		ftm->ftm_fab = libfab_newfab_init(fl);
		rc = libfab_passive_ep_create(ftm->ftm_pep, ftm);
		if (rc != FI_SUCCESS)
			return M0_RC(rc);

		m0_ref_init(&ftm->ftm_pep->fep_nep.nep_ref, 1, 
				&libfab_ep_release);
		libfab_ep_get(ftm->ftm_pep); /* TODO: Remove */
		nep = &ftm->ftm_pep->fep_nep;
		nep->nep_tm = ntm;

		m0_nep_tlink_init_at_tail(nep, &ntm->ntm_end_points);
		ftm->ftm_pep->fep_nep.nep_addr = 
					ftm->ftm_pep->fep_name.fen_str_addr;

		m0_mutex_init(&ftm->ftm_endlock);
		m0_mutex_init(&ftm->ftm_evpost);

		if (rc == FI_SUCCESS)
			rc = M0_THREAD_INIT(&ftm->ftm_poller,
					    struct m0_fab__tm *, NULL,
					    &libfab_poller, ftm,
					    "libfab_tm");
	else
		rc = M0_ERR(-ENOMEM);

	libfab_tm_evpost_lock(ftm);
	libfab_tm_unlock(ftm);
	libfab_tm_event_post(ftm, M0_NET_TM_STARTED);
	libfab_tm_lock(ftm);
	libfab_tm_evpost_unlock(ftm);

	return M0_RC(0);
}

/**
 * Stops a ma that has been started or is being started.
 *
 *
 * Used as m0_net_xprt_ops::xo_tm_stop().
 */
static int libfab_ma_stop(struct m0_net_transfer_mc *net, bool cancel)
{
	struct m0_fab__tm *tm = net->ntm_xprt_private;

	M0_PRE(net->ntm_state == M0_NET_TM_STOPPING);

	if (cancel)
		m0_net__tm_cancel(net);
	
	libfab_tm_unlock(tm);
	libfab_tm_fini(net);
	libfab_tm_event_post(tm, M0_NET_TM_STOPPED);
	libfab_tm_lock(tm);

	return M0_RC(0);
}

/**
 * Used as m0_net_xprt_ops::xo_ma_confine().
 */
static int libfab_ma_confine(struct m0_net_transfer_mc *ma,
			     const struct m0_bitmap *processors)
{
	return -ENOSYS;
}

/**
 * Returns an end-point with the given name.
 *
 * Used as m0_net_xprt_ops::xo_end_point_create().
 *
 * @see m0_net_end_point_create().
 */
static int libfab_end_point_create(struct m0_net_end_point **epp,
				   struct m0_net_transfer_mc *tm,
				   const char *name)
{
	int rc = 0;

	M0_ENTRY();

	rc = libfab_ep_find(tm, name, NULL, epp);
	if (rc != 0)
		return M0_RC(rc);

	return M0_RC(rc);
}

/**
 * Deregister a network buffer.
 *
 * Used as m0_net_xprt_ops::xo_buf_deregister().
 *
 * @see m0_net_buffer_deregister().
 */
static void libfab_buf_deregister(struct m0_net_buffer *nb)
{
	struct m0_fab__buf *fb = nb->nb_xprt_private;
	int                 i;
	int                 ret;

	M0_PRE(nb->nb_flags == M0_NET_BUF_REGISTERED &&
	       libfab_buf_invariant(fb));

	for (i = 0; i < FAB_MR_IOV_MAX; i++) {
		if (fb->fb_mr.bm_mr[i] != NULL) {
			ret = fi_close(&fb->fb_mr.bm_mr[i]->fid);
			M0_ASSERT(ret == FI_SUCCESS);
			fb->fb_mr.bm_mr[i] = NULL;
		}
	}

	libfab_buf_fini(fb);
	m0_free(fb);
	nb->nb_xprt_private = NULL;
}

/**
 * Register a network buffer that can be used for 
 * send/recv and local/remote RMA
 * Used as m0_net_xprt_ops::xo_buf_register().
 *
 * @see m0_net_buffer_register().
 */
static int libfab_buf_register(struct m0_net_buffer *nb)
{
	struct m0_fab__buf *fb;
	int                 ret = 0;

	M0_PRE(nb->nb_xprt_private == NULL);
	M0_PRE(nb->nb_dom != NULL);


	M0_ALLOC_PTR(fb);
	if (fb == NULL)
		return M0_ERR(-ENOMEM);

	fab_buf_tlink_init(fb);
	nb->nb_xprt_private = fb;
	fb->fb_nb = nb;

	return M0_RC(ret);
}

/**
 * Adds a network buffer to a ma queue.
 *
 * Used as m0_net_xprt_ops::xo_buf_add().
 *
 * @see m0_net_buffer_add().
 */
static int libfab_buf_add(struct m0_net_buffer *nb)
{
	struct m0_fab__buf       *fbp = nb->nb_xprt_private;
	struct m0_fab__tm        *ma  = libfab_buf_ma(nb);
	struct m0_fab__ep        *ep = NULL;
	struct m0_fab__active_ep *aep;
	struct iovec              iv[FAB_MR_IOV_MAX];
	m0_bcount_t               cnt = 0;
	int                       iv_cnt;
	struct m0_fab__ep_name    epname;
	int                       ret = 0;

	M0_PRE(libfab_tm_is_locked(ma) && libfab_tm_invariant(ma) &&
	       libfab_buf_invariant(fbp));
	M0_PRE(nb->nb_offset == 0); /* Do not support an offset during add. */
	M0_PRE((nb->nb_flags & M0_NET_BUF_RETAIN) == 0);

	libfab_buf_dom_reg(nb, ma->ftm_fab->fab_dom);

	switch (nb->nb_qtype) {
	case M0_NET_QT_MSG_RECV: {
		fbp->fb_length = nb->nb_length;
		iv_cnt = libfab_iov_prep(&nb->nb_buffer, iv, ARRAY_SIZE(iv),
					 nb->nb_length, &cnt);
		ret = fi_recvv(ma->ftm_rctx, iv, fbp->fb_mr.bm_desc, iv_cnt, 0,
			       fbp);
		break;
	}
	
	case M0_NET_QT_MSG_SEND: {
		M0_ASSERT(nb->nb_length <= m0_vec_count(&nb->nb_buffer.ov_vec));
		libfab_fab_ep_find(ma, NULL, nb->nb_ep->nep_addr, &ep);
		libfab_ep_get(ep);
		aep = (ep->fep_listen != NULL) ? ep->fep_listen->pep_tx_ep :
						 ep->fep_send;
		fbp->fb_txctx = ep;

		if (aep->aep_state != FAB_CONNECTED) {
			ret = libfab_conn_init(ep, ma, fbp, nb->nb_ep->nep_addr);
		} else {
			iv_cnt = libfab_iov_prep(&nb->nb_buffer, iv,
						 ARRAY_SIZE(iv), nb->nb_length,
						 &cnt);
			ret = fi_sendv(aep->aep_ep, iv, fbp->fb_mr.bm_desc,
				       iv_cnt, 0, fbp);
		}
		break;
	}

	/* For passive buffers, generate the buffer descriptor. */
	case M0_NET_QT_PASSIVE_BULK_RECV: {
		fbp->fb_length = nb->nb_length;
		ret = fi_recv(ma->ftm_rctx, &fbp->fb_rc_buf,
			      sizeof(fbp->fb_rc_buf), NULL, 0, fbp);
		if (ret == FI_SUCCESS)
			ret = libfab_bdesc_encode(fbp);
		break;
	}

	case M0_NET_QT_PASSIVE_BULK_SEND: { 
		ret = fi_recv(ma->ftm_rctx, &fbp->fb_rc_buf,
			      sizeof(fbp->fb_rc_buf), NULL, 0, fbp);
		if (ret == FI_SUCCESS)
			ret = libfab_bdesc_encode(fbp);
		break;
	}

	/* For active buffers, decode the passive buffer descriptor */
	case M0_NET_QT_ACTIVE_BULK_RECV: {
		memset(&epname, 0, sizeof(epname));
		libfab_bdesc_decode(&nb->nb_desc, &fbp->fb_rem_key, &epname);
		libfab_fab_ep_find(ma, &epname, NULL, &ep);
		libfab_ep_get(ep);
		fbp->fb_txctx = ep;
		aep = (ep->fep_listen != NULL) ? ep->fep_listen->pep_tx_ep :
						 ep->fep_send;
		fbp->fb_length = nb->nb_length;

		if (aep->aep_state != FAB_CONNECTED) {
			ret = libfab_conn_init(ep, ma, fbp, NULL);
		}
		else {
			iv_cnt = libfab_iov_prep(&nb->nb_buffer, iv,
						 ARRAY_SIZE(iv), nb->nb_length,
						 &cnt);
			ret = fi_readv(aep->aep_ep, iv, fbp->fb_mr.bm_desc,
				       iv_cnt, 0, 0, fbp->fb_rem_key, fbp);
		}
		break;
	}

	case M0_NET_QT_ACTIVE_BULK_SEND: {
		memset(&epname, 0, sizeof(epname));
		libfab_bdesc_decode(&nb->nb_desc, &fbp->fb_rem_key, &epname);
		libfab_fab_ep_find(ma, &epname, NULL, &ep);
		libfab_ep_get(ep);
		fbp->fb_txctx = ep;
		aep = (ep->fep_listen != NULL) ? ep->fep_listen->pep_tx_ep :
						 ep->fep_send;
		if (aep->aep_state != FAB_CONNECTED) {
			ret = libfab_conn_init(ep, ma, fbp, NULL);
		}
		else {
			iv_cnt = libfab_iov_prep(&nb->nb_buffer, iv,
						 ARRAY_SIZE(iv), nb->nb_length,
						 &cnt);
			ret = fi_writev(aep->aep_ep, iv, fbp->fb_mr.bm_desc,
					iv_cnt, 0, 0, fbp->fb_rem_key, fbp);
		}
		break;
	}

	default:
		M0_IMPOSSIBLE("invalid queue type: %x", nb->nb_qtype);
		break;
	}

	return M0_RC(ret);
}

/**
 * Cancels a buffer operation..
 *
 * Used as m0_net_xprt_ops::xo_buf_del().
 *
 * @see m0_net_buffer_del().
 */
static void libfab_buf_del(struct m0_net_buffer *nb)
{
	struct m0_fab__buf *buf = nb->nb_xprt_private;
	struct m0_fab__tm  *ma = libfab_buf_ma(nb);
	int                 i;
	int                 ret;

	M0_PRE(libfab_tm_is_locked(ma) && libfab_tm_invariant(ma) &&
	       libfab_buf_invariant(buf));
	nb->nb_flags |= M0_NET_BUF_CANCELLED;

	/*TODO: fi_cancel*/
	// fi_cancel(ma->ftm_fab.fab_rctx)
	for (i = 0; i < FAB_MR_IOV_MAX; i++) {
		if (buf->fb_mr.bm_mr[i] != NULL) {
			ret = fi_close(&buf->fb_mr.bm_mr[i]->fid);
			M0_ASSERT(ret == FI_SUCCESS);
			buf->fb_mr.bm_mr[i] = NULL;
		}
	}

	libfab_buf_done(buf, -ECANCELED);
}

static int libfab_bev_deliver_sync(struct m0_net_transfer_mc *ma)
{
	return 0;
}

static void libfab_bev_deliver_all(struct m0_net_transfer_mc *ma)
{

}

static bool libfab_bev_pending(struct m0_net_transfer_mc *ma)
{
	return false;
}

static void libfab_bev_notify(struct m0_net_transfer_mc *ma,
			      struct m0_chan *chan)
{

}

/**
 * Maximal number of bytes in a buffer.
 *
 * Used as m0_net_xprt_ops::xo_get_max_buffer_size()
 *
 * @see m0_net_domain_get_max_buffer_size()
 */
static m0_bcount_t libfab_get_max_buf_size(const struct m0_net_domain *dom)
{
	/*TODO: Get proper value from libfabric domain attribute */
	return M0_BCOUNT_MAX/2; /* 1048576; */
}

/**
 * Maximal number of bytes in a buffer segment.
 *
 * Used as m0_net_xprt_ops::xo_get_max_buffer_segment_size()
 *
 * @see m0_net_domain_get_max_buffer_segment_size()
 */
static m0_bcount_t libfab_get_max_buf_seg_size(const struct m0_net_domain *dom)
{
	/*TODO: Get proper value from libfabric domain attribute */
	return M0_BCOUNT_MAX / 2; /* 4096; */
}

/**
 * Maximal number of segments in a buffer
 *
 * Used as m0_net_xprt_ops::xo_get_max_buffer_segments()
 *
 * @see m0_net_domain_get_max_buffer_segments()
 */
static int32_t libfab_get_max_buf_segments(const struct m0_net_domain *dom)
{
	/*TODO: Get proper value from libfabric domain attribute */
	return  INT32_MAX/2; /*256; */
}

/**
 * Maximal number of bytes in a buffer descriptor.
 *
 * Used as m0_net_xprt_ops::xo_get_max_buffer_desc_size()
 *
 * @see m0_net_domain_get_max_buffer_desc_size()
 */
static m0_bcount_t libfab_get_max_buf_desc_size(const struct m0_net_domain *dom)
{
	return (sizeof(uint64_t) * 2);
}

static struct m0_fab__tm *libfab_buf_ma(struct m0_net_buffer *buf)
{
	return buf->nb_tm->ntm_xprt_private;
}

static const struct m0_net_xprt_ops libfab_xprt_ops = {
	.xo_dom_init                    = &libfab_dom_init,
	.xo_dom_fini                    = &libfab_dom_fini,
	.xo_tm_init                     = &libfab_ma_init,
	.xo_tm_confine                  = &libfab_ma_confine,
	.xo_tm_start                    = &libfab_ma_start,
	.xo_tm_stop                     = &libfab_ma_stop,
	.xo_tm_fini                     = &libfab_ma_fini,
	.xo_end_point_create            = &libfab_end_point_create,
	.xo_buf_register                = &libfab_buf_register,
	.xo_buf_deregister              = &libfab_buf_deregister,
	.xo_buf_add                     = &libfab_buf_add,
	.xo_buf_del                     = &libfab_buf_del,
	.xo_bev_deliver_sync            = &libfab_bev_deliver_sync,
	.xo_bev_deliver_all             = &libfab_bev_deliver_all,
	.xo_bev_pending                 = &libfab_bev_pending,
	.xo_bev_notify                  = &libfab_bev_notify,
	.xo_get_max_buffer_size         = &libfab_get_max_buf_size,
	.xo_get_max_buffer_segment_size = &libfab_get_max_buf_seg_size,
	.xo_get_max_buffer_segments     = &libfab_get_max_buf_segments,
	.xo_get_max_buffer_desc_size    = &libfab_get_max_buf_desc_size
};

struct m0_net_xprt m0_net_libfab_xprt = {
	.nx_name = "libfab",
	.nx_ops  = &libfab_xprt_ops
};
M0_EXPORTED(m0_net_libfab_xprt);

#undef M0_TRACE_SUBSYSTEM

/** @} end of netlibfab group */

/*
 *  Local variables:
 *  c-indentation-style: "K&R"
 *  c-basic-offset: 8
 *  tab-width: 8
 *  fill-column: 80
 *  scroll-step: 1
 *  End:
 */
/*
 * vim: tabstop=8 shiftwidth=8 noexpandtab textwidth=80 nowrap
 */