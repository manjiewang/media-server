#include <stdlib.h>
#include "cstringext.h"
#include "ctypedef.h"
#include "rtp-h264-unpack.h"
#include "rtp-packet.h"
#include "rtp-header.h"

#define H264_NAL(v)	(v & 0x1F)
#define FU_START(v) (v & 0x80)
#define FU_END(v)	(v & 0x40)
#define FU_NAL(v)	(v & 0x1F)

struct rtp_h264_unpack_t
{
	rtp_h264_unpack_onnal callback;
	void* cbparam;

	uint16_t seq; // rtp seq

	void* ptr;
	int size, capacity;
};

void* rtp_h264_unpack_create(rtp_h264_unpack_onnal callback, void* param)
{
	struct rtp_h264_unpack_t *unpacker;
	unpacker = (struct rtp_h264_unpack_t *)malloc(sizeof(*unpacker));
	if(!unpacker)
		return NULL;

	memset(unpacker, 0, sizeof(*unpacker));
	unpacker->callback = callback;
	unpacker->cbparam = param;
	return unpacker;
}

void rtp_h264_unpack_destroy(void* p)
{
	struct rtp_h264_unpack_t *unpacker;
	unpacker = (struct rtp_h264_unpack_t *)p;

	if(unpacker->ptr)
		free(unpacker->ptr);
	free(unpacker);
}

static int rtp_h264_unpack_stap_a(struct rtp_h264_unpack_t *unpacker, const void* data, int bytes)
{
	// 5.7.1. Single-Time Aggregation Packet (STAP) (p20)
	unsigned char stapnal;
	uint16_t nallen;
	const unsigned char* ptr;
	ptr = (const unsigned char*)data;
	stapnal = ptr[0];

	++ptr;
	nallen = 0;
	for(bytes -= 1; bytes > 2; bytes -= nallen + 2)
	{
		unsigned char nal;

		nallen = be_read_uint16(ptr);
		if(nallen + 2 > bytes)
		{
			assert(0);
			return -1; // error
		}

		nal = ptr[2];
		assert(H264_NAL(nal) > 0 && H264_NAL(nal) < 24);

		unpacker->callback(unpacker->cbparam, nal, ptr+3, nallen-1);

		ptr = ptr + nallen + 2; // next NALU
	}

	return 0;
}

static int rtp_h264_unpack_stap_b(struct rtp_h264_unpack_t *unpacker, const void* data, int bytes)
{
	// 5.7.1. Single-Time Aggregation Packet (STAP)
	unsigned char stapnal;
	uint16_t don;
	uint16_t nallen;
	const unsigned char* ptr;
	ptr = (const unsigned char*)data;

	stapnal = ptr[0];
	don = be_read_uint16(ptr + 1);

	ptr += 3;
	nallen = 0;
	for(bytes -= 1; bytes > 2; bytes -= nallen + 2)
	{
		unsigned char nal;

		nallen = be_read_uint16(ptr);
		if(nallen + 2> bytes)
		{
			assert(0);
			return -1; // error
		}

		nal = ptr[2];
		assert(H264_NAL(nal) > 0 && H264_NAL(nal) < 24);
		unpacker->callback(unpacker->cbparam, nal, ptr+3, nallen-1);

		ptr = ptr + nallen + 2; // next NALU
	}

	return 0;
}

static int rtp_h264_unpack_mtap16(struct rtp_h264_unpack_t *unpacker, const void* data, int bytes)
{
	// 5.7.2. Multi-Time Aggregation Packets (MTAPs)
	unsigned char mtapnal;
	uint16_t donb;
	uint16_t nallen;
	const unsigned char* ptr;
	ptr = (const unsigned char*)data;

	mtapnal = ptr[0];
	donb = be_read_uint16(ptr+1);

	ptr += 3;
	for(bytes -= 1; bytes > 6; bytes -= nallen + 2)
	{
		unsigned char nal, dond;
		uint16_t timestamp;

		nallen = be_read_uint16(ptr);
		if(nallen + 2 > bytes)
		{
			assert(0);
			return -1; // error
		}

		assert(nallen > 4);
		dond = ptr[2];
		timestamp = be_read_uint16(ptr+3);

		nal = ptr[5];
		assert(H264_NAL(nal) > 0 && H264_NAL(nal) < 24);
		unpacker->callback(unpacker->cbparam, nal, ptr+6, nallen-4);

		ptr = ptr + nallen + 2; // next NALU
	}

	return 0;
}

static int rtp_h264_unpack_mtap24(struct rtp_h264_unpack_t *unpacker, const void* data, int bytes)
{
	// 5.7.2. Multi-Time Aggregation Packets (MTAPs)
	unsigned char mtapnal;
	uint16_t donb;
	uint16_t nallen;
	const unsigned char* ptr;
	ptr = (const unsigned char*)data;

	mtapnal = ptr[0];
	donb = be_read_uint16(ptr + 1);

	ptr += 3;
	for(bytes -= 1; bytes > 2; bytes -= nallen + 2)
	{
		unsigned char nal, dond;
		unsigned int timestamp;
		const unsigned short *p;
		p = (const unsigned short *)ptr;

		nallen = be_read_uint16(ptr);
		if(nallen > bytes - 2)
		{
			assert(0);
			return -1; // error
		}

		assert(nallen > 5);
		ptr = (const unsigned char*)(p + 1);
		dond = ptr[0];
		timestamp = (ptr[1] << 16) | (ptr[2] << 8) | ptr[3];

		nal = ptr[4];
		assert(H264_NAL(nal) > 0 && H264_NAL(nal) < 24);
		unpacker->callback(unpacker->cbparam, nal, ptr+5, nallen-5);

		ptr = ptr + nallen; // next NALU
	}

	return 0;
}

static int rtp_h264_unpack_fu_a(struct rtp_h264_unpack_t *unpacker, const void* data, int bytes)
{
	unsigned char fuindicator, fuheader;
	const unsigned char* ptr;
	ptr = (const unsigned char*)data;

	fuindicator = ptr[0];
	fuheader = ptr[1];

	if(FU_START(fuheader))
	{
		assert(0 == unpacker->size);
		unpacker->size = 0;
	}
	else
	{
		assert(0 < unpacker->size);
	}

	if(unpacker->size + bytes - 2 > unpacker->capacity)
	{
		int size = unpacker->size + bytes * 2;
		unpacker->ptr = realloc(unpacker->ptr, size);
		if(!unpacker->ptr)
		{
			unpacker->capacity = 0;
			unpacker->size = 0;
			return -1;
		}
		unpacker->capacity = size;
	}

	memmove((char*)unpacker->ptr + unpacker->size, ptr + 2, bytes - 2);
	unpacker->size += bytes - 2;

	if(FU_END(fuheader))
	{
		unsigned char nal;
		nal = (fuindicator & 0xE0) | (fuheader & 0x1F);
		assert(H264_NAL(nal) > 0 && H264_NAL(nal) < 24);
		unpacker->callback(unpacker->cbparam, nal, unpacker->ptr, unpacker->size);
		unpacker->size = 0; // reset
	}

	return 0;
}

static int rtp_h264_unpack_fu_b(struct rtp_h264_unpack_t *unpacker, const void* data, int bytes)
{
	unsigned char fuindicator, fuheader;
	unsigned short don;
	const unsigned char* ptr;
	ptr = (const unsigned char*)data;

	fuindicator = ptr[0];
	fuheader = ptr[1];
	don = be_read_uint16(ptr + 2);

	if(FU_START(fuheader))
	{
		assert(0 == unpacker->size);
		unpacker->size = 0;
	}
	else
	{
		assert(0 < unpacker->size);
	}

	if(unpacker->size + bytes - 4 > unpacker->capacity)
	{
		int size = unpacker->size + bytes * 2;
		unpacker->ptr = realloc(unpacker->ptr, size);
		if(!unpacker->ptr)
		{
			unpacker->capacity = 0;
			unpacker->size = 0;
			return -1;
		}
		unpacker->capacity = size;
	}

	memmove((char*)unpacker->ptr + unpacker->size, ptr + 4, bytes - 4);
	unpacker->size += bytes - 4;

	if(FU_END(fuheader))
	{
		unsigned char nal;
		nal = (fuindicator & 0xE0) | (fuheader & 0x1F);
		assert(H264_NAL(nal) > 0 && H264_NAL(nal) < 24);
		unpacker->callback(unpacker->cbparam, nal, unpacker->ptr, unpacker->size);
		unpacker->size = 0; // reset
	}

	return 0;
}

int rtp_h264_unpack_input(void* p, const void* packet, size_t bytes)
{
	rtp_packet_t pkt;
	unsigned char nal;
	struct rtp_h264_unpack_t *unpacker;

	unpacker = (struct rtp_h264_unpack_t *)p;
	if(0 != rtp_packet_deserialize(&pkt, packet, bytes) || pkt.payloadlen < 1)
		return -1;

	if((uint16_t)pkt.rtp.seq != unpacker->seq+1)
	{
		// packet lost
		unpacker->size = 0; // clear fu-a/b flags
	}

	unpacker->seq = (uint16_t)pkt.rtp.seq;

	assert(pkt.payloadlen > 0);
	nal = ((unsigned char *)pkt.payload)[0];

	switch(nal & 0x1F)
	{
	case 0: // reserved
	case 31: // reserved
		assert(0);
		break;

	case 24: // STAP-A
		rtp_h264_unpack_stap_a(unpacker, pkt.payload, pkt.payloadlen);
		break;
	case 25: // STAP-B
		assert(0);
		//rtp_h264_unpack_stap_b(unpacker, pkt.payload, pkt.payloadlen);
		break;
	case 26: // MTAP16
		assert(0);
		//rtp_h264_unpack_mtap16(unpacker, pkt.payload, pkt.payloadlen);
		break;
	case 27: // MTAP24
		assert(0);
		//rtp_h264_unpack_mtap24(unpacker, pkt.payload, pkt.payloadlen);
		break;
	case 28: // FU-A
		rtp_h264_unpack_fu_a(unpacker, pkt.payload, pkt.payloadlen);
		break;
	case 29: // FU-B
		assert(0);
		//rtp_h264_unpack_fu_b(unpacker, pkt.payload, pkt.payloadlen);
		break;

	default: // 1-23 NAL unit
		unpacker->callback(unpacker->cbparam, nal, (const unsigned char*)pkt.payload + 1, pkt.payloadlen-1);
		break;
	}

	return 1;
}
