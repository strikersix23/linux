// SPDX-License-Identifier: GPL-2.0-or-later
/* connection-level event handling
 *
 * Copyright (C) 2007 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/net.h>
#include <linux/skbuff.h>
#include <linux/errqueue.h>
#include <net/sock.h>
#include <net/af_rxrpc.h>
#include <net/ip.h>
#include "ar-internal.h"

/*
 * Set the completion state on an aborted connection.
 */
static bool rxrpc_set_conn_aborted(struct rxrpc_connection *conn,
				   s32 abort_code, int err,
				   enum rxrpc_call_completion compl)
{
	bool aborted = false;

	if (conn->state != RXRPC_CONN_ABORTED) {
		spin_lock_irq(&conn->state_lock);
		if (conn->state != RXRPC_CONN_ABORTED) {
			conn->abort_code = abort_code;
			conn->error	 = err;
			conn->completion = compl;
			/* Order the abort info before the state change. */
			smp_store_release(&conn->state, RXRPC_CONN_ABORTED);
			set_bit(RXRPC_CONN_DONT_REUSE, &conn->flags);
			set_bit(RXRPC_CONN_EV_ABORT_CALLS, &conn->events);
			aborted = true;
		}
		spin_unlock_irq(&conn->state_lock);
	}

	return aborted;
}

/*
 * Mark a socket buffer to indicate that the connection it's on should be aborted.
 */
int rxrpc_abort_conn(struct rxrpc_connection *conn, struct sk_buff *skb,
		     s32 abort_code, int err, enum rxrpc_abort_reason why)
{

	u32 cid = conn->proto.cid, call = 0, seq = 0;

	if (skb) {
		struct rxrpc_skb_priv *sp = rxrpc_skb(skb);

		cid  = sp->hdr.cid;
		call = sp->hdr.callNumber;
		seq  = sp->hdr.seq;
	}

	if (rxrpc_set_conn_aborted(conn, abort_code, err,
				   RXRPC_CALL_LOCALLY_ABORTED)) {
		trace_rxrpc_abort(0, why, cid, call, seq, abort_code, err);
		rxrpc_poke_conn(conn, rxrpc_conn_get_poke_abort);
	}
	return -EPROTO;
}

/*
 * Mark a connection as being remotely aborted.
 */
static void rxrpc_input_conn_abort(struct rxrpc_connection *conn,
				   struct sk_buff *skb)
{
	trace_rxrpc_rx_conn_abort(conn, skb);
	rxrpc_set_conn_aborted(conn, skb->priority, -ECONNABORTED,
			       RXRPC_CALL_REMOTELY_ABORTED);
}

/*
 * Retransmit terminal ACK or ABORT of the previous call.
 */
void rxrpc_conn_retransmit_call(struct rxrpc_connection *conn,
				struct sk_buff *skb,
				unsigned int channel)
{
	struct rxrpc_skb_priv *sp = skb ? rxrpc_skb(skb) : NULL;
	struct rxrpc_channel *chan;
	struct msghdr msg;
	struct kvec iov[3];
	struct {
		struct rxrpc_wire_header whdr;
		union {
			__be32 abort_code;
			struct rxrpc_ackpacket ack;
		};
	} __attribute__((packed)) pkt;
	struct rxrpc_acktrailer trailer;
	size_t len;
	int ret, ioc;
	u32 serial, max_mtu, if_mtu, call_id, padding;

	_enter("%d", conn->debug_id);

	if (sp && sp->hdr.type == RXRPC_PACKET_TYPE_ACK) {
		if (skb_copy_bits(skb, sizeof(struct rxrpc_wire_header),
				  &pkt.ack, sizeof(pkt.ack)) < 0)
			return;
		if (pkt.ack.reason == RXRPC_ACK_PING_RESPONSE)
			return;
	}

	chan = &conn->channels[channel];

	/* If the last call got moved on whilst we were waiting to run, just
	 * ignore this packet.
	 */
	call_id = chan->last_call;
	if (skb && call_id != sp->hdr.callNumber)
		return;

	msg.msg_name	= &conn->peer->srx.transport;
	msg.msg_namelen	= conn->peer->srx.transport_len;
	msg.msg_control	= NULL;
	msg.msg_controllen = 0;
	msg.msg_flags	= 0;

	iov[0].iov_base	= &pkt;
	iov[0].iov_len	= sizeof(pkt.whdr);
	iov[1].iov_base	= &padding;
	iov[1].iov_len	= 3;
	iov[2].iov_base	= &trailer;
	iov[2].iov_len	= sizeof(trailer);

	serial = rxrpc_get_next_serial(conn);

	pkt.whdr.epoch		= htonl(conn->proto.epoch);
	pkt.whdr.cid		= htonl(conn->proto.cid | channel);
	pkt.whdr.callNumber	= htonl(call_id);
	pkt.whdr.serial		= htonl(serial);
	pkt.whdr.seq		= 0;
	pkt.whdr.type		= chan->last_type;
	pkt.whdr.flags		= conn->out_clientflag;
	pkt.whdr.userStatus	= 0;
	pkt.whdr.securityIndex	= conn->security_ix;
	pkt.whdr._rsvd		= 0;
	pkt.whdr.serviceId	= htons(conn->service_id);

	len = sizeof(pkt.whdr);
	switch (chan->last_type) {
	case RXRPC_PACKET_TYPE_ABORT:
		pkt.abort_code	= htonl(chan->last_abort);
		iov[0].iov_len += sizeof(pkt.abort_code);
		len += sizeof(pkt.abort_code);
		ioc = 1;
		break;

	case RXRPC_PACKET_TYPE_ACK:
		if_mtu = conn->peer->if_mtu - conn->peer->hdrsize;
		if (conn->peer->ackr_adv_pmtud) {
			max_mtu = umax(conn->peer->max_data, rxrpc_rx_mtu);
		} else {
			if_mtu = umin(1444, if_mtu);
			max_mtu = if_mtu;
		}
		pkt.ack.bufferSpace	= 0;
		pkt.ack.maxSkew		= htons(skb ? skb->priority : 0);
		pkt.ack.firstPacket	= htonl(chan->last_seq + 1);
		pkt.ack.previousPacket	= htonl(chan->last_seq);
		pkt.ack.serial		= htonl(skb ? sp->hdr.serial : 0);
		pkt.ack.reason		= skb ? RXRPC_ACK_DUPLICATE : RXRPC_ACK_IDLE;
		pkt.ack.nAcks		= 0;
		trailer.maxMTU		= htonl(max_mtu);
		trailer.ifMTU		= htonl(if_mtu);
		trailer.rwind		= htonl(rxrpc_rx_window_size);
		trailer.jumbo_max	= 0;
		pkt.whdr.flags		|= RXRPC_SLOW_START_OK;
		padding			= 0;
		iov[0].iov_len += sizeof(pkt.ack);
		len += sizeof(pkt.ack) + 3 + sizeof(trailer);
		ioc = 3;

		trace_rxrpc_tx_ack(chan->call_debug_id, serial,
				   ntohl(pkt.ack.firstPacket),
				   ntohl(pkt.ack.serial),
				   pkt.ack.reason, 0, rxrpc_rx_window_size,
				   rxrpc_propose_ack_retransmit);
		break;

	default:
		return;
	}

	ret = kernel_sendmsg(conn->local->socket, &msg, iov, ioc, len);
	conn->peer->last_tx_at = ktime_get_seconds();
	if (ret < 0)
		trace_rxrpc_tx_fail(chan->call_debug_id, serial, ret,
				    rxrpc_tx_point_call_final_resend);
	else
		trace_rxrpc_tx_packet(chan->call_debug_id, &pkt.whdr,
				      rxrpc_tx_point_call_final_resend);

	_leave("");
}

/*
 * pass a connection-level abort onto all calls on that connection
 */
static void rxrpc_abort_calls(struct rxrpc_connection *conn)
{
	struct rxrpc_call *call;
	int i;

	_enter("{%d},%x", conn->debug_id, conn->abort_code);

	for (i = 0; i < RXRPC_MAXCALLS; i++) {
		call = conn->channels[i].call;
		if (call) {
			rxrpc_see_call(call, rxrpc_call_see_conn_abort);
			rxrpc_set_call_completion(call,
						  conn->completion,
						  conn->abort_code,
						  conn->error);
			rxrpc_poke_call(call, rxrpc_call_poke_conn_abort);
		}
	}

	_leave("");
}

/*
 * mark a call as being on a now-secured channel
 * - must be called with BH's disabled.
 */
static void rxrpc_call_is_secure(struct rxrpc_call *call)
{
	if (call && __test_and_clear_bit(RXRPC_CALL_CONN_CHALLENGING, &call->flags))
		rxrpc_notify_socket(call);
}

/*
 * connection-level Rx packet processor
 */
static int rxrpc_process_event(struct rxrpc_connection *conn,
			       struct sk_buff *skb)
{
	struct rxrpc_skb_priv *sp = rxrpc_skb(skb);
	int ret;

	if (conn->state == RXRPC_CONN_ABORTED)
		return -ECONNABORTED;

	_enter("{%d},{%u,%%%u},", conn->debug_id, sp->hdr.type, sp->hdr.serial);

	switch (sp->hdr.type) {
	case RXRPC_PACKET_TYPE_CHALLENGE:
		ret = conn->security->respond_to_challenge(conn, skb);
		sp->chall.conn = NULL;
		rxrpc_put_connection(conn, rxrpc_conn_put_challenge_input);
		return ret;

	case RXRPC_PACKET_TYPE_RESPONSE:
		ret = conn->security->verify_response(conn, skb);
		if (ret < 0)
			return ret;

		ret = conn->security->init_connection_security(
			conn, conn->key->payload.data[0]);
		if (ret < 0)
			return ret;

		spin_lock_irq(&conn->state_lock);
		if (conn->state == RXRPC_CONN_SERVICE_CHALLENGING)
			conn->state = RXRPC_CONN_SERVICE;
		spin_unlock_irq(&conn->state_lock);

		if (conn->state == RXRPC_CONN_SERVICE) {
			/* Offload call state flipping to the I/O thread.  As
			 * we've already received the packet, put it on the
			 * front of the queue.
			 */
			sp->poke_conn = rxrpc_get_connection(
				conn, rxrpc_conn_get_poke_secured);
			skb->mark = RXRPC_SKB_MARK_SERVICE_CONN_SECURED;
			rxrpc_get_skb(skb, rxrpc_skb_get_conn_secured);
			skb_queue_head(&conn->local->rx_queue, skb);
			rxrpc_wake_up_io_thread(conn->local);
		}
		return 0;

	default:
		WARN_ON_ONCE(1);
		return -EPROTO;
	}
}

/*
 * set up security and issue a challenge
 */
static void rxrpc_secure_connection(struct rxrpc_connection *conn)
{
	if (conn->security->issue_challenge(conn) < 0)
		rxrpc_abort_conn(conn, NULL, RX_CALL_DEAD, -ENOMEM,
				 rxrpc_abort_nomem);
}

/*
 * Process delayed final ACKs that we haven't subsumed into a subsequent call.
 */
void rxrpc_process_delayed_final_acks(struct rxrpc_connection *conn, bool force)
{
	unsigned long j = jiffies, next_j;
	unsigned int channel;
	bool set;

again:
	next_j = j + LONG_MAX;
	set = false;
	for (channel = 0; channel < RXRPC_MAXCALLS; channel++) {
		struct rxrpc_channel *chan = &conn->channels[channel];
		unsigned long ack_at;

		if (!test_bit(RXRPC_CONN_FINAL_ACK_0 + channel, &conn->flags))
			continue;

		ack_at = chan->final_ack_at;
		if (time_before(j, ack_at) && !force) {
			if (time_before(ack_at, next_j)) {
				next_j = ack_at;
				set = true;
			}
			continue;
		}

		if (test_and_clear_bit(RXRPC_CONN_FINAL_ACK_0 + channel,
				       &conn->flags))
			rxrpc_conn_retransmit_call(conn, NULL, channel);
	}

	j = jiffies;
	if (time_before_eq(next_j, j))
		goto again;
	if (set)
		rxrpc_reduce_conn_timer(conn, next_j);
}

/*
 * connection-level event processor
 */
static void rxrpc_do_process_connection(struct rxrpc_connection *conn)
{
	struct sk_buff *skb;
	int ret;

	if (test_and_clear_bit(RXRPC_CONN_EV_CHALLENGE, &conn->events))
		rxrpc_secure_connection(conn);

	/* go through the conn-level event packets, releasing the ref on this
	 * connection that each one has when we've finished with it */
	while ((skb = skb_dequeue(&conn->rx_queue))) {
		rxrpc_see_skb(skb, rxrpc_skb_see_conn_work);
		ret = rxrpc_process_event(conn, skb);
		switch (ret) {
		case -ENOMEM:
		case -EAGAIN:
			skb_queue_head(&conn->rx_queue, skb);
			rxrpc_queue_conn(conn, rxrpc_conn_queue_retry_work);
			break;
		default:
			rxrpc_free_skb(skb, rxrpc_skb_put_conn_work);
			break;
		}
	}
}

void rxrpc_process_connection(struct work_struct *work)
{
	struct rxrpc_connection *conn =
		container_of(work, struct rxrpc_connection, processor);

	rxrpc_see_connection(conn, rxrpc_conn_see_work);

	if (__rxrpc_use_local(conn->local, rxrpc_local_use_conn_work)) {
		rxrpc_do_process_connection(conn);
		rxrpc_unuse_local(conn->local, rxrpc_local_unuse_conn_work);
	}
}

/*
 * post connection-level events to the connection
 * - this includes challenges, responses, some aborts and call terminal packet
 *   retransmission.
 */
static void rxrpc_post_packet_to_conn(struct rxrpc_connection *conn,
				      struct sk_buff *skb)
{
	_enter("%p,%p", conn, skb);

	rxrpc_get_skb(skb, rxrpc_skb_get_conn_work);
	skb_queue_tail(&conn->rx_queue, skb);
	rxrpc_queue_conn(conn, rxrpc_conn_queue_rx_work);
}

/*
 * Post a CHALLENGE packet to the socket of one of a connection's calls so that
 * it can get application data to include in the packet, possibly querying
 * userspace.
 */
static bool rxrpc_post_challenge(struct rxrpc_connection *conn,
				 struct sk_buff *skb)
{
	struct rxrpc_skb_priv *sp = rxrpc_skb(skb);
	struct rxrpc_call *call = NULL;
	struct rxrpc_sock *rx;
	bool respond = false;

	sp->chall.conn =
		rxrpc_get_connection(conn, rxrpc_conn_get_challenge_input);

	if (!conn->security->challenge_to_recvmsg) {
		rxrpc_post_packet_to_conn(conn, skb);
		return true;
	}

	rcu_read_lock();

	for (int i = 0; i < ARRAY_SIZE(conn->channels); i++) {
		if (conn->channels[i].call) {
			call = conn->channels[i].call;
			rx = rcu_dereference(call->socket);
			if (!rx) {
				call = NULL;
				continue;
			}

			respond = true;
			if (test_bit(RXRPC_SOCK_MANAGE_RESPONSE, &rx->flags))
				break;
			call = NULL;
		}
	}

	if (!respond) {
		rcu_read_unlock();
		rxrpc_put_connection(conn, rxrpc_conn_put_challenge_input);
		sp->chall.conn = NULL;
		return false;
	}

	if (call)
		rxrpc_notify_socket_oob(call, skb);
	rcu_read_unlock();

	if (!call)
		rxrpc_post_packet_to_conn(conn, skb);
	return true;
}

/*
 * Input a connection-level packet.
 */
bool rxrpc_input_conn_packet(struct rxrpc_connection *conn, struct sk_buff *skb)
{
	struct rxrpc_skb_priv *sp = rxrpc_skb(skb);

	switch (sp->hdr.type) {
	case RXRPC_PACKET_TYPE_BUSY:
		/* Just ignore BUSY packets for now. */
		return true;

	case RXRPC_PACKET_TYPE_ABORT:
		if (rxrpc_is_conn_aborted(conn))
			return true;
		rxrpc_input_conn_abort(conn, skb);
		rxrpc_abort_calls(conn);
		return true;

	case RXRPC_PACKET_TYPE_CHALLENGE:
		rxrpc_see_skb(skb, rxrpc_skb_see_oob_challenge);
		if (rxrpc_is_conn_aborted(conn)) {
			if (conn->completion == RXRPC_CALL_LOCALLY_ABORTED)
				rxrpc_send_conn_abort(conn);
			return true;
		}
		if (!conn->security->validate_challenge(conn, skb))
			return false;
		return rxrpc_post_challenge(conn, skb);

	case RXRPC_PACKET_TYPE_RESPONSE:
		if (rxrpc_is_conn_aborted(conn)) {
			if (conn->completion == RXRPC_CALL_LOCALLY_ABORTED)
				rxrpc_send_conn_abort(conn);
			return true;
		}
		rxrpc_post_packet_to_conn(conn, skb);
		return true;

	default:
		WARN_ON_ONCE(1);
		return true;
	}
}

/*
 * Input a connection event.
 */
void rxrpc_input_conn_event(struct rxrpc_connection *conn, struct sk_buff *skb)
{
	unsigned int loop;

	if (test_and_clear_bit(RXRPC_CONN_EV_ABORT_CALLS, &conn->events))
		rxrpc_abort_calls(conn);

	if (conn->tx_response) {
		struct sk_buff *skb;

		spin_lock_irq(&conn->local->lock);
		skb = conn->tx_response;
		conn->tx_response = NULL;
		spin_unlock_irq(&conn->local->lock);

		if (conn->state != RXRPC_CONN_ABORTED)
			rxrpc_send_response(conn, skb);
		rxrpc_free_skb(skb, rxrpc_skb_put_response);
	}

	if (skb) {
		switch (skb->mark) {
		case RXRPC_SKB_MARK_SERVICE_CONN_SECURED:
			if (conn->state != RXRPC_CONN_SERVICE)
				break;

			for (loop = 0; loop < RXRPC_MAXCALLS; loop++)
				rxrpc_call_is_secure(conn->channels[loop].call);
			break;
		}
	}

	/* Process delayed ACKs whose time has come. */
	if (conn->flags & RXRPC_CONN_FINAL_ACK_MASK)
		rxrpc_process_delayed_final_acks(conn, false);
}

/*
 * Post a RESPONSE message to the I/O thread for transmission.
 */
void rxrpc_post_response(struct rxrpc_connection *conn, struct sk_buff *skb)
{
	struct rxrpc_skb_priv *sp = rxrpc_skb(skb);
	struct rxrpc_local *local = conn->local;
	struct sk_buff *old;

	_enter("%x", sp->resp.challenge_serial);

	spin_lock_irq(&local->lock);
	old = conn->tx_response;
	if (old) {
		struct rxrpc_skb_priv *osp = rxrpc_skb(skb);

		/* Always go with the response to the most recent challenge. */
		if (after(sp->resp.challenge_serial, osp->resp.challenge_serial))
			conn->tx_response = old;
		else
			old = skb;
	} else {
		conn->tx_response = skb;
	}
	spin_unlock_irq(&local->lock);
	rxrpc_poke_conn(conn, rxrpc_conn_get_poke_response);
}
