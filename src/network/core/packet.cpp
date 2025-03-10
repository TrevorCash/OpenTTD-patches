/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file packet.cpp Basic functions to create, fill and read packets.
 */

#include "../../stdafx.h"
#include "../../string_func.h"
#include "../../string_func_extra.h"
#include "../../command_type.h"

#include "packet.h"

#include "../../safeguards.h"

/**
 * Create a packet that is used to read from a network socket.
 * @param cs                The socket handler associated with the socket we are reading from.
 * @param limit             The maximum size of packets to accept.
 * @param initial_read_size The initial amount of data to transfer from the socket into the
 *                          packet. This defaults to just the required bytes to determine the
 *                          packet's size. That default is the wanted for streams such as TCP
 *                          as you do not want to read data of the next packet yet. For UDP
 *                          you need to read the whole packet at once otherwise you might
 *                          loose some the data of the packet, so there you pass the maximum
 *                          size for the packet you expect from the network.
 */
Packet::Packet(NetworkSocketHandler *cs, size_t limit, size_t initial_read_size) : pos(0), limit(limit)
{
	assert(cs != nullptr);

	this->cs = cs;
	this->buffer.resize(initial_read_size);
}

/**
 * Creates a packet to send
 * @param type  The type of the packet to send
 * @param limit The maximum number of bytes the packet may have. Default is COMPAT_MTU.
 *              Be careful of compatibility with older clients/servers when changing
 *              the limit as it might break things if the other side is not expecting
 *              much larger packets than what they support.
 */
Packet::Packet(PacketType type, size_t limit) : pos(0), limit(limit), cs(nullptr)
{
	this->ResetState(type);
}

void Packet::ResetState(PacketType type)
{
	this->cs = nullptr;
	this->buffer.clear();

	/* Allocate space for the the size so we can write that in just before sending the packet. */
	this->Send_uint16(0);
	this->Send_uint8(type);
}

/**
 * Writes the packet size from the raw packet from packet->size
 */
void Packet::PrepareToSend()
{
	assert(this->cs == nullptr);

	this->buffer[0] = GB(this->Size(), 0, 8);
	this->buffer[1] = GB(this->Size(), 8, 8);

	this->pos  = 0; // We start reading from here
	this->buffer.shrink_to_fit();
}

/**
 * Is it safe to write to the packet, i.e. didn't we run over the buffer?
 * @param bytes_to_write The amount of bytes we want to try to write.
 * @return True iff the given amount of bytes can be written to the packet.
 */
bool Packet::CanWriteToPacket(size_t bytes_to_write)
{
	return this->Size() + bytes_to_write < this->limit;
}

/*
 * The next couple of functions make sure we can send
 *  uint8, uint16, uint32 and uint64 endian-safe
 *  over the network. The least significant bytes are
 *  sent first.
 *
 *  So 0x01234567 would be sent as 67 45 23 01.
 *
 * A bool is sent as a uint8 where zero means false
 *  and non-zero means true.
 */

/**
 * Package a boolean in the packet.
 * @param data The data to send.
 */
void Packet::Send_bool(bool data)
{
	this->Send_uint8(data ? 1 : 0);
}

/**
 * Package a 8 bits integer in the packet.
 * @param data The data to send.
 */
void Packet::Send_uint8(uint8 data)
{
	assert(this->CanWriteToPacket(sizeof(data)));
	this->buffer.emplace_back(data);
}

/**
 * Package a 16 bits integer in the packet.
 * @param data The data to send.
 */
void Packet::Send_uint16(uint16 data)
{
	assert(this->CanWriteToPacket(sizeof(data)));
	this->buffer.insert(this->buffer.end(), {
		(uint8)GB(data,  0, 8),
		(uint8)GB(data,  8, 8),
	});
}

/**
 * Package a 32 bits integer in the packet.
 * @param data The data to send.
 */
void Packet::Send_uint32(uint32 data)
{
	assert(this->CanWriteToPacket(sizeof(data)));
	this->buffer.insert(this->buffer.end(), {
		(uint8)GB(data,  0, 8),
		(uint8)GB(data,  8, 8),
		(uint8)GB(data, 16, 8),
		(uint8)GB(data, 24, 8),
	});
}

/**
 * Package a 64 bits integer in the packet.
 * @param data The data to send.
 */
void Packet::Send_uint64(uint64 data)
{
	assert(this->CanWriteToPacket(sizeof(data)));
	this->buffer.insert(this->buffer.end(), {
		(uint8)GB(data,  0, 8),
		(uint8)GB(data,  8, 8),
		(uint8)GB(data, 16, 8),
		(uint8)GB(data, 24, 8),
		(uint8)GB(data, 32, 8),
		(uint8)GB(data, 40, 8),
		(uint8)GB(data, 48, 8),
		(uint8)GB(data, 56, 8),
	});
}

/**
 * Sends a string over the network. It sends out
 * the string + '\0'. No size-byte or something.
 * @param data The string to send
 */
void Packet::Send_string(const char *data)
{
	assert(data != nullptr);
	/* Length of the string + 1 for the '\0' termination. */
	assert(this->CanWriteToPacket(strlen(data) + 1));
	while (this->buffer.emplace_back(*data++) != '\0') {}
}

/**
 * Send as many of the bytes as possible in the packet. This can mean
 * that it is possible that not all bytes are sent. To cope with this
 * the function returns the amount of bytes that were actually sent.
 * @param begin The begin of the buffer to send.
 * @param end   The end of the buffer to send.
 * @return The number of bytes that were added to this packet.
 */
size_t Packet::Send_bytes(const byte *begin, const byte *end)
{
	size_t amount = std::min<size_t>(end - begin, this->limit - this->Size());
	this->buffer.insert(this->buffer.end(), begin, begin + amount);
	return amount;
}

/**
 * Sends a binary data over the network.
 * @param data The data to send
 */
void Packet::Send_binary(const char *data, const size_t size)
{
	assert(data != nullptr);
	assert(this->CanWriteToPacket(size));
	this->buffer.insert(this->buffer.end(), data, data + size);
}


/*
 * Receiving commands
 * Again, the next couple of functions are endian-safe
 *  see the comment before Send_bool for more info.
 */


/**
 * Is it safe to read from the packet, i.e. didn't we run over the buffer?
 * In case \c close_connection is true, the connection will be closed when one would
 * overrun the buffer. When it is false, the connection remains untouched.
 * @param bytes_to_read    The amount of bytes we want to try to read.
 * @param close_connection Whether to close the connection if one cannot read that amount.
 * @return True if that is safe, otherwise false.
 */
bool Packet::CanReadFromPacket(size_t bytes_to_read, bool close_connection)
{
	/* Don't allow reading from a quit client/client who send bad data */
	if (this->cs->HasClientQuit()) return false;

	/* Check if variable is within packet-size */
	if (this->pos + bytes_to_read > this->Size()) {
		if (close_connection) this->cs->NetworkSocketHandler::CloseConnection();
		return false;
	}

	return true;
}

/**
 * Check whether the packet, given the position of the "write" pointer, has read
 * enough of the packet to contain its size.
 * @return True iff there is enough data in the packet to contain the packet's size.
 */
bool Packet::HasPacketSizeData() const
{
	return this->pos >= sizeof(PacketSize);
}

/**
 * Get the number of bytes in the packet.
 * When sending a packet this is the size of the data up to that moment.
 * When receiving a packet (before PrepareToRead) this is the allocated size for the data to be read.
 * When reading a packet (after PrepareToRead) this is the full size of the packet.
 * @return The packet's size.
 */
size_t Packet::Size() const
{
	return this->buffer.size();
}

size_t Packet::ReadRawPacketSize() const
{
	return (size_t)this->buffer[0] + ((size_t)this->buffer[1] << 8);
}

/**
 * Reads the packet size from the raw packet and stores it in the packet->size
 * @return True iff the packet size seems plausible.
 */
bool Packet::ParsePacketSize()
{
	assert(this->cs != nullptr);
	size_t size = (size_t)this->buffer[0];
	size       += (size_t)this->buffer[1] << 8;

	/* If the size of the packet is less than the bytes required for the size and type of
	 * the packet, or more than the allowed limit, then something is wrong with the packet.
	 * In those cases the packet can generally be regarded as containing garbage data. */
	if (size < sizeof(PacketSize) + sizeof(PacketType) || size > this->limit) return false;

	this->buffer.resize(size);
	this->pos = sizeof(PacketSize);
	return true;
}

/**
 * Prepares the packet so it can be read
 */
void Packet::PrepareToRead()
{
	/* Put the position on the right place */
	this->pos = sizeof(PacketSize);
}

/**
 * Get the \c PacketType from this packet.
 * @return The packet type.
 */
PacketType Packet::GetPacketType() const
{
	assert(this->Size() >= sizeof(PacketSize) + sizeof(PacketType));
	return static_cast<PacketType>(buffer[sizeof(PacketSize)]);
}

/**
 * Read a boolean from the packet.
 * @return The read data.
 */
bool Packet::Recv_bool()
{
	return this->Recv_uint8() != 0;
}

/**
 * Read a 8 bits integer from the packet.
 * @return The read data.
 */
uint8 Packet::Recv_uint8()
{
	uint8 n;

	if (!this->CanReadFromPacket(sizeof(n), true)) return 0;

	n = this->buffer[this->pos++];
	return n;
}

/**
 * Read a 16 bits integer from the packet.
 * @return The read data.
 */
uint16 Packet::Recv_uint16()
{
	uint16 n;

	if (!this->CanReadFromPacket(sizeof(n), true)) return 0;

	n  = (uint16)this->buffer[this->pos++];
	n += (uint16)this->buffer[this->pos++] << 8;
	return n;
}

/**
 * Read a 32 bits integer from the packet.
 * @return The read data.
 */
uint32 Packet::Recv_uint32()
{
	uint32 n;

	if (!this->CanReadFromPacket(sizeof(n), true)) return 0;

	n  = (uint32)this->buffer[this->pos++];
	n += (uint32)this->buffer[this->pos++] << 8;
	n += (uint32)this->buffer[this->pos++] << 16;
	n += (uint32)this->buffer[this->pos++] << 24;
	return n;
}

/**
 * Read a 64 bits integer from the packet.
 * @return The read data.
 */
uint64 Packet::Recv_uint64()
{
	uint64 n;

	if (!this->CanReadFromPacket(sizeof(n), true)) return 0;

	n  = (uint64)this->buffer[this->pos++];
	n += (uint64)this->buffer[this->pos++] << 8;
	n += (uint64)this->buffer[this->pos++] << 16;
	n += (uint64)this->buffer[this->pos++] << 24;
	n += (uint64)this->buffer[this->pos++] << 32;
	n += (uint64)this->buffer[this->pos++] << 40;
	n += (uint64)this->buffer[this->pos++] << 48;
	n += (uint64)this->buffer[this->pos++] << 56;
	return n;
}

/**
 * Reads a string till it finds a '\0' in the stream.
 * @param buffer The buffer to put the data into.
 * @param size   The size of the buffer.
 * @param settings The string validation settings.
 */
void Packet::Recv_string(char *buffer, size_t size, StringValidationSettings settings)
{
	PacketSize pos;
	char *bufp = buffer;
	const char *last = buffer + size - 1;

	/* Don't allow reading from a closed socket */
	if (cs->HasClientQuit()) return;

	pos = this->pos;
	while (--size > 0 && pos < this->Size() && (*buffer++ = this->buffer[pos++]) != '\0') {}

	if (size == 0 || pos == this->Size()) {
		*buffer = '\0';
		/* If size was sooner to zero then the string in the stream
		 *  skip till the \0, so than packet can be read out correctly for the rest */
		while (pos < this->Size() && this->buffer[pos] != '\0') pos++;
		pos++;
	}
	this->pos = pos;

	str_validate(bufp, last, settings);
}

/**
 * Get the amount of bytes that are still available for the Transfer functions.
 * @return The number of bytes that still have to be transfered.
 */
size_t Packet::RemainingBytesToTransfer() const
{
	return this->Size() - this->pos;
}

/**
 * Reads a string till it finds a '\0' in the stream.
 * @param buffer The buffer to put the data into.
 * @param settings The string validation settings.
 */
void Packet::Recv_string(std::string &buffer, StringValidationSettings settings)
{
	/* Don't allow reading from a closed socket */
	if (cs->HasClientQuit()) return;

	size_t length = ttd_strnlen((const char *)(this->buffer.data() + this->pos), this->Size() - this->pos - 1);
	buffer.assign((const char *)(this->buffer.data() + this->pos), length);
	this->pos += (uint)length + 1;
	str_validate_inplace(buffer, settings);
}

/**
 * Reads binary data.
 * @param buffer The buffer to put the data into.
 * @param size   The size of the data.
 */
void Packet::Recv_binary(char *buffer, size_t size)
{
	if (!this->CanReadFromPacket(size, true)) return;

	memcpy(buffer, &this->buffer[this->pos], size);
	this->pos += (PacketSize) size;
}

/**
 * Reads binary data.
 * @param buffer The buffer to put the data into.
 * @param size   The size of the data.
 */
void Packet::Recv_binary(std::string &buffer, size_t size)
{
	if (!this->CanReadFromPacket(size, true)) return;

	buffer.assign((const char *) &this->buffer[this->pos], size);
	this->pos += (PacketSize) size;
}
