/* $Id$ */

/**
 * @file tcp.cpp Basic functions to receive and send TCP packets.
 */

#ifdef ENABLE_NETWORK

#include "../../stdafx.h"
#include "../../debug.h"
#include "../../openttd.h"
#include "../../variables.h"

#include "../network_internal.h"
#include "packet.h"
#include "tcp.h"

#include "table/strings.h"
#include "../../oldpool_func.h"

/** Make very sure the preconditions given in network_type.h are actually followed */
assert_compile(MAX_CLIENT_SLOTS == (MAX_CLIENT_SLOTS >> NCI_BITS_PER_POOL_BLOCK) << NCI_BITS_PER_POOL_BLOCK);
assert_compile(MAX_CLIENT_SLOTS > MAX_CLIENTS);

typedef ClientIndex NetworkClientSocketID;
DEFINE_OLD_POOL_GENERIC(NetworkClientSocket, NetworkClientSocket);

NetworkClientSocket::NetworkClientSocket(ClientID client_id)
{
	this->sock              = INVALID_SOCKET;
	this->client_id         = client_id;
	this->status            = STATUS_INACTIVE;
}

NetworkClientSocket::~NetworkClientSocket()
{
	if (this->sock != INVALID_SOCKET) closesocket(this->sock);
	this->writable = false;
	this->has_quit = true;

	/* Free all pending and partially received packets */
	while (this->packet_queue != NULL) {
		Packet *p = this->packet_queue->next;
		delete this->packet_queue;
		this->packet_queue = p;
	}
	delete this->packet_recv;
	this->packet_recv = NULL;

	while (this->command_queue != NULL) {
		CommandPacket *p = this->command_queue->next;
		free(this->command_queue);
		this->command_queue = p;
	}

	this->sock = INVALID_SOCKET;
	this->client_id = INVALID_CLIENT_ID;
	this->status = STATUS_INACTIVE;
}

/**
 * Functions to help NetworkRecv_Packet/NetworkSend_Packet a bit
 *  A socket can make errors. When that happens this handles what to do.
 * For clients: close connection and drop back to main-menu
 * For servers: close connection and that is it
 * @return the new status
 * TODO: needs to be splitted when using client and server socket packets
 */
NetworkRecvStatus NetworkClientSocket::CloseConnection()
{
	/* Clients drop back to the main menu */
	if (!_network_server && _networking) {
		_switch_mode = SM_MENU;
		_networking = false;
		extern StringID _switch_mode_errorstr;
		_switch_mode_errorstr = STR_NETWORK_ERR_LOSTCONNECTION;

		return NETWORK_RECV_STATUS_CONN_LOST;
	}

	NetworkCloseClient(this);
	return NETWORK_RECV_STATUS_OKAY;
}

/**
 * This function puts the packet in the send-queue and it is send as
 * soon as possible. This is the next tick, or maybe one tick later
 * if the OS-network-buffer is full)
 * @param packet the packet to send
 */
void NetworkClientSocket::Send_Packet(Packet *packet)
{
	Packet *p;
	assert(packet != NULL);

	packet->PrepareToSend();

	/* Locate last packet buffered for the client */
	p = this->packet_queue;
	if (p == NULL) {
		/* No packets yet */
		this->packet_queue = packet;
	} else {
		/* Skip to the last packet */
		while (p->next != NULL) p = p->next;
		p->next = packet;
	}
}

/**
 * Sends all the buffered packets out for this client. It stops when:
 *   1) all packets are send (queue is empty)
 *   2) the OS reports back that it can not send any more
 *      data right now (full network-buffer, it happens ;))
 *   3) sending took too long
 */
bool NetworkClientSocket::Send_Packets()
{
	ssize_t res;
	Packet *p;

	/* We can not write to this socket!! */
	if (!this->writable) return false;
	if (!this->IsConnected()) return false;

	p = this->packet_queue;
	while (p != NULL) {
		res = send(this->sock, (const char*)p->buffer + p->pos, p->size - p->pos, 0);
		if (res == -1) {
			int err = GET_LAST_ERROR();
			if (err != EWOULDBLOCK) {
				/* Something went wrong.. close client! */
				DEBUG(net, 0, "send failed with error %d", err);
				this->CloseConnection();
				return false;
			}
			return true;
		}
		if (res == 0) {
			/* Client/server has left us :( */
			this->CloseConnection();
			return false;
		}

		p->pos += res;

		/* Is this packet sent? */
		if (p->pos == p->size) {
			/* Go to the next packet */
			this->packet_queue = p->next;
			delete p;
			p = this->packet_queue;
		} else {
			return true;
		}
	}

	return true;
}

/**
 * Receives a packet for the given client
 * @param status the variable to store the status into
 * @return the received packet (or NULL when it didn't receive one)
 */
Packet *NetworkClientSocket::Recv_Packet(NetworkRecvStatus *status)
{
	ssize_t res;
	Packet *p;

	*status = NETWORK_RECV_STATUS_OKAY;

	if (!this->IsConnected()) return NULL;

	if (this->packet_recv == NULL) {
		this->packet_recv = new Packet(this);
		if (this->packet_recv == NULL) error("Failed to allocate packet");
	}

	p = this->packet_recv;

	/* Read packet size */
	if (p->pos < sizeof(PacketSize)) {
		while (p->pos < sizeof(PacketSize)) {
		/* Read the size of the packet */
			res = recv(this->sock, (char*)p->buffer + p->pos, sizeof(PacketSize) - p->pos, 0);
			if (res == -1) {
				int err = GET_LAST_ERROR();
				if (err != EWOULDBLOCK) {
					/* Something went wrong... (104 is connection reset by peer) */
					if (err != 104) DEBUG(net, 0, "recv failed with error %d", err);
					*status = this->CloseConnection();
					return NULL;
				}
				/* Connection would block, so stop for now */
				return NULL;
			}
			if (res == 0) {
				/* Client/server has left */
				*status = this->CloseConnection();
				return NULL;
			}
			p->pos += res;
		}

		/* Read the packet size from the received packet */
		p->ReadRawPacketSize();

		if (p->size > SEND_MTU) {
			*status = this->CloseConnection();
			return NULL;
		}
	}

	/* Read rest of packet */
	while (p->pos < p->size) {
		res = recv(this->sock, (char*)p->buffer + p->pos, p->size - p->pos, 0);
		if (res == -1) {
			int err = GET_LAST_ERROR();
			if (err != EWOULDBLOCK) {
				/* Something went wrong... (104 is connection reset by peer) */
				if (err != 104) DEBUG(net, 0, "recv failed with error %d", err);
				*status = this->CloseConnection();
				return NULL;
			}
			/* Connection would block */
			return NULL;
		}
		if (res == 0) {
			/* Client/server has left */
			*status = this->CloseConnection();
			return NULL;
		}

		p->pos += res;
	}

	/* Prepare for receiving a new packet */
	this->packet_recv = NULL;

	p->PrepareToRead();
	return p;
}

bool NetworkClientSocket::IsPacketQueueEmpty()
{
	return this->packet_queue == NULL;
}


#endif /* ENABLE_NETWORK */
