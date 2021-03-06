/*
 * SerialToTCPBridgeProtocol.c
 *
 *  Created on: Jul 29, 2016
 *      Author: Roan
 */

#include "SerialToTCPBridgeProtocol.h"

// Private Methods
static void startTX(Client* c, bool timeout, bool kickstart, bool ack)
{
	static volatile uint8_t txState = TX_IDLE;
	static int packetLength;

	// If we are waiting for an Ack and a timeout occured
	// we resend the last publish packet.
	if (timeout)
	{
		if (txState == TX_WAIT)
		{
			// Ack did not arrive in time, resend last pub
			txState = TX_IDLE;
		} else
		{
			return;
		}
	}

	// If we write to the tx buffer and the uart is idling
	// we should initiate the transmit process.
	if (kickstart && (txState != TX_IDLE))
	{
		return;
	}

	// If we received an Ack, move on.
	if (ack && (txState == TX_WAIT))
	{
		c->txBuf.pH += (uint8_t)packetLength;
		c->txBuf.isFull = false;
		txState = TX_IDLE;
	}

	switch (txState)
	{
	// Start sending the next packet
	case TX_IDLE:
		if (c->txBuf.available(&c->txBuf) > 0)
		{
			packetLength = (int)(c->txBuf.Buf[c->txBuf.pH]) + 1;
			if ((int)c->txBuf.pH + packetLength > 256) // check if packet surpasses buffer end
			{
				if (HAL_UART_Transmit_IT(c->peripheral_UART, &c->txBuf.Buf[c->txBuf.pH], 256 - c->txBuf.pH) != HAL_OK)
					return; // TODO: react on this
				c->lastOutAct = HAL_GetTick();
				txState = TX_BUSY;
			} else
			{
				if (HAL_UART_Transmit_IT(c->peripheral_UART, &c->txBuf.Buf[c->txBuf.pH], (uint16_t)packetLength) != HAL_OK)
					return;
				c->lastOutAct = HAL_GetTick();
				if ((c->txBuf.Buf[c->txBuf.pH + 1] & 0x7F) == PROTOCOL_PUBLISH)
				{
					c->expectedAckSeq = (c->txBuf.Buf[c->txBuf.pH + 1] & 0x80) > 0;
					c->ackOutstanding = true;
					txState = TX_WAIT;
				} else
				{
					c->txBuf.pH += (uint8_t)packetLength;
					c->txBuf.isFull = false;
				}
			}
		}
		break;
	// Complete sending of current packet
	case TX_BUSY:
		if (HAL_UART_Transmit_IT(c->peripheral_UART, c->txBuf.Buf, packetLength + c->txBuf.pH - 256) != HAL_OK)
			return;
		c->lastOutAct = HAL_GetTick();
		if ((c->txBuf.Buf[c->txBuf.pH + 1] & 0x7F) == PROTOCOL_PUBLISH)
		{
			c->expectedAckSeq = (c->txBuf.Buf[c->txBuf.pH + 1] & 0x80) > 0;
			c->ackOutstanding = true;
			txState = TX_WAIT;
		} else
		{
			c->txBuf.pH += (uint8_t)packetLength;
			txState = TX_IDLE;
			c->txBuf.isFull = false;
		}
		break;
	case TX_WAIT:
		break;
	}
}

static bool writePacket(Client* c, uint8_t command, uint8_t* payload, uint8_t pLength)
{
	c->workBuffer[0] = pLength + 5;
	c->workBuffer[1] = command;
	if (payload != NULL)
	{
		for (uint8_t i = 2; i < pLength + 2; i++)
		{
			c->workBuffer[i] = payload[i - 2];
		}
	}
	uint32_t crcCode = HAL_CRC_Calculate(c->peripheral_CRC, (uint32_t*)(c->workBuffer), pLength + 2);
	crcCode ^= 0xffffffff;
	c->workBuffer[pLength + 2] = crcCode & 0x000000FF;
	c->workBuffer[pLength + 3] = (crcCode & 0x0000FF00) >> 8;
	c->workBuffer[pLength + 4] = (crcCode & 0x00FF0000) >> 16;
	c->workBuffer[pLength + 5] = (crcCode & 0xFF000000) >> 24;

	// see if packet will fit in transmit buffer
	if ((int)(pLength) + 6 > 256 - c->txBuf.available(&c->txBuf))
	{
		return false;
	}

	// write packet into tx buffer
	for (int i = 0; i < pLength + 6; i++)
	{
		c->txBuf.Buf[c->txBuf.pT++] = c->workBuffer[i];
	}
	c->txBuf.isFull = (c->txBuf.pT == c->txBuf.pH);

	startTX(c, false, true, false);

	return true;
}

static size_t publish(Client* c, uint8_t* payload, uint8_t pLength)
{
	static bool pubSequence = false;

	uint8_t cmd = PROTOCOL_PUBLISH;
	if (pubSequence)
	{
		cmd |= 0x80;
	}
	pubSequence = !pubSequence;

	if (!writePacket(c, cmd, payload, pLength))
	{
		return 0;
	}
	return pLength;
}

static void rxHandlePacket(Client* c, uint8_t* packetStart)
{
	bool rxSeqFlag = (packetStart[1] & 0x80) > 0;
	switch (packetStart[1] & 0x7F)
	{
	// Connection established with destination
	case PROTOCOL_CONNACK:
		if (packetStart[0] == 5)
		{
			c->state = STATE_CONNECTED;
		}
		break;
	// Incoming data
	case PROTOCOL_PUBLISH:
		writePacket(c, PROTOCOL_ACK | (packetStart[1] & 0x80), NULL, 0);
		if (rxSeqFlag == c->expectedRxSeqFlag)
		{
			c->expectedRxSeqFlag = !c->expectedRxSeqFlag;
			if (packetStart[0] > 5)
			{
				for (uint8_t i = 0; i < packetStart[0] - 5; i++)
				{
					c->readBuf.Buf[c->readBuf.pT++] = packetStart[2 + i];
				}
				c->readBuf.isFull = (c->readBuf.pH == c->readBuf.pT);
			}
		}
		break;
	// Protocol Acknowledge
	case PROTOCOL_ACK:
		if (c->ackOutstanding)
		{
			if (rxSeqFlag == c->expectedAckSeq)
			{
				c->ackOutstanding = false;
				startTX(c, false, false, true);
			}
		}
		break;
	}
}

// Callback hook ups
void uartTxCompleteCallback(Client* c)
{
	startTX(c, false, false, false);
}

// TODO: Packet RX timeout, buffer full check?
void uartRxCompleteCallback(Client* c)
{
	static uint8_t packetCount = 0;
	static uint8_t rxState = RX_PACKET_IDLE;

	c->rxBuffer[packetCount++] = c->rxByte;
	switch (rxState)
	{
	case RX_PACKET_IDLE:
		rxState = RX_PACKET_GOTLENGTH;
		break;
	case RX_PACKET_GOTLENGTH:
		rxState = RX_PACKET_GOTCOMMAND;
		break;
	case RX_PACKET_GOTCOMMAND:
		; // has to be here, otherwise 'deceleration after label' error
		uint8_t packetLength = c->rxBuffer[0];
		if (packetCount == packetLength + 1) // Got rest of packet
		{
			packetCount = 0;
			// Integrity checking
			uint32_t crcRx = c->rxBuffer[packetLength - 3] | (c->rxBuffer[packetLength - 2] << 8)
					| (c->rxBuffer[packetLength - 1] << 16) | (c->rxBuffer[packetLength] << 24);
			uint32_t crcCode = HAL_CRC_Calculate(c->peripheral_CRC, (uint32_t*)(c->rxBuffer), packetLength - 3);
			crcCode ^= 0xffffffff;
			if (crcRx == crcCode) // validate packet
			{
				rxHandlePacket(c, c->rxBuffer);
			}
			rxState = RX_PACKET_IDLE;
		}
		break;
	}
	HAL_UART_Receive_IT(c->peripheral_UART, &c->rxByte, 1);
}

void tickInterupt(Client* c)
{
	// If we are waiting for an Ack,
	// we need to resend the last publish after timeout
	// TODO: Need to change this to a one time pulse using timer, achieves 1 asynchronous callback.
	if (c->ackOutstanding)
	{
		uint32_t now = HAL_GetTick();
		if (now - c->lastOutAct > 500)
		{
			startTX(c, true, false, false);
		}
	}
}

// Public Methods
static int availablePublic(const void* c)
{
	Client* self = (Client*)c;

	return self->readBuf.available(&self->readBuf);
}

static int readPublic(const void* c)
{
	Client* self = (Client*)c;

	if (!self->available(c))
	{
		return -1;
	}
	uint8_t ch = self->readBuf.Buf[self->readBuf.pH++];
	self->readBuf.isFull = false;
	return ch;
}

static int connectPublic(const void* c, uint8_t ip[4], uint16_t port)
{
	Client* self = (Client*)c;
	HAL_UART_Receive_IT(self->peripheral_UART, &self->rxByte, 1); // start rx on hardware uart interface
	uint8_t destination[6] = { ip[0], ip[1], ip[2], ip[3], (uint8_t)port, (uint8_t)(port >> 8) };
	writePacket(self, PROTOCOL_CONNECT, destination, 6);
	while (self->state != STATE_CONNECTED)
	{
		uint32_t now = HAL_GetTick();
		if (now - self->lastInAct >= 5000)
		{
			return -1;
		}
	}
	self->lastInAct = HAL_GetTick();
	return 1;
	/*	SUCCESS 1
	 TIMED_OUT -1
	 INVALID_SERVER -2
	 TRUNCATED -3
	 INVALID_RESPONSE -4
	 */
}

static uint8_t connectedPublic(const void* c)
{
	Client* self = (Client*)c;

	if (self->state == STATE_CONNECTED)
	{
		return 1;
	}
	return 0;
}

static void flushPublic(const void* c)
{
	;
}

static void stopPublic(const void* c)
{
	;
}

static size_t writePublic(const void* c, uint8_t* payload, uint8_t pLength)
{
	Client* self = (Client*)c;

	return publish(self, payload, pLength);
}

// Constructor
void newClient(Client* c, UART_HandleTypeDef* uartUnit, CRC_HandleTypeDef* crcUnit)
{
	c->peripheral_UART = uartUnit;
	c->peripheral_CRC = crcUnit;

	newByteBuffer(&c->txBuf);
	newByteBuffer(&c->readBuf);
	c->ackOutstanding = false;
	c->expectedAckSeq = false;
	c->expectedRxSeqFlag = false;

	c->state = STATE_DISCONNECTED;

	// Arduino Client interface API
	c->connect = connectPublic;
	c->connected = connectedPublic;
	c->available = availablePublic;
	c->read = readPublic;
	c->write = writePublic;
	c->flush = flushPublic;
	c->stop = stopPublic;
}
