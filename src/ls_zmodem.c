/**********************************************************
 * File: ls_zmodem.c
 * Created at Sun Oct 29 18:51:46 2000 by lev // lev@serebryakov.spb.ru
 * 
 * $Id: ls_zmodem.c,v 1.6 2000/11/10 12:37:21 lev Exp $
 **********************************************************/
/*

   ZModem file transfer protocol. Written from scratches.
   Support CRC16, CRC32, RLE Encoding, variable header, ZedZap (big blocks).
   Global variables, common functions.

*/
#include "mailer.h"
#include "defs.h"
#include "ls_zmodem.h"
#include "qipc.h"

/* Common variables */
int ls_txHdr[LSZ_MAXHLEN];	/* Sended header */
int ls_rxHdr[LSZ_MAXHLEN];	/* Receiver header */
int ls_GotZDLE;				/* We seen DLE as last character */
int ls_GotHexNibble;		/* We seen one hex digit as last character */
int ls_Protocol;			/* Plain/ZedZap/DirZap */
int ls_CANCount;			/* Count of CANs to go */
int ls_Garbage;				/* Count of garbage characters */

/* Variables to control sender */
int ls_txWinSize;			/* Receiver Window/Buffer size (0 for streaming) */
int ls_txCould;				/* Receiver could fullduplex/streamed IO */
int ls_txCurBlockSize;		/* Current block size */
int ls_txMaxBlockSize;		/* Maximal block size */
int ls_txLastSent;			/* Last sent character -- for escaping */
long ls_txLastACK;			/* Last ACKed byte */
long ls_txLastRepos;		/* Last requested byte */

/* Variables to control receiver */


/* Special table to FAST calculate header type */
/*          CRC32,VAR,RLE */
int HEADER_TYPE[2][2][2] = {{{ZBIN,-1},{ZVBIN,-1}},
							{{ZBIN32,ZBINR32},{ZVBIN32,ZVBINR32}}};

/* Hex digits */
char HEX_DIGITS[] = "0123456789abcdef";

/* Functions */

/* Send binary header. Use proper CRC, send var. len. if could */
int ls_zsendbhdr(int frametype, int len, char *hdr)
{
	long crc = LSZ_INIT_CRC;
	int type;
	int n;

	/* First, calculate packet header byte */
	if ((type = HEADER_TYPE[(ls_Protocol & LSZ_OPTCRC32)==LSZ_OPTCRC32][(ls_Protocol & LSZ_OPTVHDR)==LSZ_OPTVHDR][(ls_Protocol & LSZ_OPTRLE)==LSZ_OPTRLE]) < 0) {
		log("zmodem: invalid options set, %s, %s, %s",
			(ls_Protocol & LSZ_OPTCRC32)?"crc32":"crc16",
			(ls_Protocol & LSZ_OPTVHDR)?"varh":"fixh",
			(ls_Protocol & LSZ_OPTRLE)?"RLE":"Plain");
		return ERROR;
	}

	/* Send *<DLE> and packet type */
	BUFCHAR(ZPAD);BUFCHAR(ZDLE);BUFCHAR(type);
	if (ls_Protocol & LSZ_OPTVHDR) ls_sendchar(len);			/* Send length of header, if needed */

	ls_sendchar(frametype);							/* Send type of frame */
	crc = LSZ_UPDATE_CRC(frametype,crc);
	/* Send whole header */
	for (n=len; --n >= 0; ++hdr) {
		ls_senzdle(*hdr);
		crc = LSZ_UPDATE_CRC((0xFF & *hdr),crc);
	}
	crc = LSZ_FINISH_CRC(crc);
	if (ls_Protocol & LSZ_OPTCRC32) {
		crc = LTOI(crc);
		for (n=4; --n >= 0;) { ls_sendchar(crc & 0xFF); crc >>= 8; }
	} else {
		crc = STOI(crc & 0xFFFF);
		ls_sendchar(crc >> 8);
		ls_sendchar(crc & 0xFF);
	}
	/* Clean buffer, do real send */
	return BUFFLUSH();
}

/* Send HEX header. Use CRC16, send var. len. if could */
int ls_zsendhhdr(int frametype, int len, char *hdr)
{
	long crc = LSZ_INIT_CRC16;
	int n;

	/* Send **<DLE> */
	BUFCHAR(ZPAD); BUFCHAR(ZPAD); BUFCHAR(ZDLE);

	/* Send header type */
	if (ls_Protocol & LSZ_OPTVHDR) {
		BUFCHAR(ZVHEX);
		ls_sendhex(len);
	} else {
		BUFCHAR(ZHEX);
	}

	ls_sendhex(frametype);
	crc = LSZ_UPDATE_CRC16(frametype,crc);
	/* Send whole header */
	for (n=len; --n >= 0; ++hdr) {
		ls_sendchar(*hdr);
		crc = LSZ_UPDATE_CRC16((0xFF & *hdr),crc);
	}
	crc = LSZ_FINISH_CRC16(crc);
	crc = STOI(crc & 0xFFFF);
	ls_sendhex(crc >> 8);
	ls_sendhex(crc & 0xFF);
	/* Clean buffer, do real send */
	return BUFLUSH();
}

int ls_zrecvhdr(char *hdr, int *hlen, int timeout)
{
	static enum rhSTATE {
		rhInit,				/* Start state */
		rhZPAD,				/* ZPAD got (*) */
		rhZDLE,				/* We got ZDLE */
		rhZBIN,
		rhZHEX,
		rhZBIN32,
		rhZBINR32,
		rhZVBIN,
		rhZVHEX,
		rhZVBIN32,
		rhZVBINR32,
		rhBYTE,
		rhCRC
	} state = rhInit;
	static enum rhREADMODE {
		rm7BIT,
		rmZDLE,
		rmHEX
	} readmode = rm7BIT;

	static int frametype = LSZ_ERROR;
	static int crcl = 2;
	static int crcgot = 0;
	static long incrc = 0;
	static long crc = 0;
	static int len = 0;
	static int got = 0;
	static int inhex = 0;
	int t = time(NULL);
	int c;
	int res;

	if(rhInit == state) {
		crc = 0;
		crcl = 2;
		crcgot = 0;
		incrc = LSZ_INIT_CRC16;
		len = 0;
		got = 0;
		inhex = 0;
		readmode = rm7BIT;
	}

	while(OK == (res = HASDATA(timeout))) {
		switch(readmode) {
		case rm7BIT: c = ls_read7bit(0); break;
		case rmZDLE: c = ls_readzdle(0); break;
		case rmHEX: c = ls_readhex(0); break;
		}
		if(c < 0) return c;
		c &= 0xFF;
		timeout -= (time(NULL) - t); t = time(NULL);

		switch(state) {
		case rhInit:
			if(ZPAD == c) { state = rhZPAD; }
			else { ls_Garbage++; }
			break;
		case rhZPAD:
			switch(c) {
			case ZPAD: break;
			case ZDLE: state = rhZDLE; break;
			default: ls_Garbage++; state = rhInit; break;
			}
			break;
		case rhZDLE:
			switch(c) {
			case ZBIN: state = rhZBIN; frametype = c; readmode = rmZDLE; break;
			case ZHEX: state = rhZHEX; frametype = c; readmode = rmHEX; break;
			case ZBIN32: state = rhZBIN32; frametype = c; readmode = rmZDLE; break;
			case ZBINR32: state = rhZBINR32; frametype = c; readmode = rmZDLE; break;
			case ZVBIN: state = rhZVBIN; frametype = c; readmode = rmZDLE; break;
			case ZVHEX: state = rhZVHEX; frametype = c; readmode = rmHEX; break;
			case ZVBIN32: state = rhZVBIN32; frametype = c; readmode = rmZDLE; break;
			case ZVBINR32: state = rhZVBINR32; frametype = c; readmode = rmZDLE; break;
			default: ls_Garbage++; state = rhInit; readmode = rm7BIT; break;
			}
			break;
		case rhZVBIN32:
		case rhZVBINR32:
			crcl = 4; /* Fall throught */
		case rhZVBIN:
		case rhZVHEX:
			len = c;
			state = rhBYTE;
			break;
		case rhZBIN32:
		case rhZBINR32:
			crcl = 4; /* Fall throught */
		case rhZBIN:
		case rhZHEX:
			len = 4;
			state = rhBYTE;
			break;
		case rhBYTE:
			hdr[got] = c;
			if(++got == len) state = rhCRC;
			if(2 == crcl) { incrc = LSZ_UPDATE_CRC16(c,incrc); } 
			else { incrc = LSZ_UPDATE_CRC32(c,incrc); }
			break;
		case rhCRC:
			crc <<= 8;
			crc |= c;
			if(++crcgot == crcl)  { /* Crc finished */
				state = rhInit;
				ls_Garbage = 0;
				incrc = LSZ_FINISH_CRC(incrc);
				if(2 == crcl) { crc = STOH(crc & 0xFFFF); }
				else { crc = LTOH(crc); }
				if (incrc != crc) return LSZ_BADCRC;
				*hlen = got;
				return frametype;
			}
			break;
		default:
			break;
		}
	}
	return res;
}

/* Send data block, with CRC and framing */
int ls_senddata(char *data, int len, int frame)
{
	long crc = LSZ_INIT_CRC;
	int n;

	for(; len--; data++) {
		ls_sendchar(*data);
		crc = LSZ_UPDATE_CRC(*data,crc);
	}
	crc = LSZ_FINISH_CRC(crc);
	if (ls_Protocol & LSZ_OPTCRC32) {
		crc = LTOI(crc);
		for (n=4; --n >= 0;) { ls_sendchar(crc & 0xFF); crc >>= 8; }
	} else {
		crc = STOI(crc & 0xFFFF);
		ls_sendchar(crc >> 8);
		ls_sendchar(crc & 0xFF);
	}
	ls_sendchar(frame);
	if((ls_Protocol & LSZ_OPTDIRZAP) == 0 && ZCRCW == frame) BUFCHAR(XON);
	return BUFFLUSH();
}

/* Receive data block, return frame type or error (may be -- timeout) */
int ls_recvdata(char *data, int *len, int timeout, int crc32)
{
	int c;
	int t = time(NULL);
	int crcl = crc32?4:2;
	int crcgot = 0;
	int got = 0;
	long incrc = crc32?LSZ_INIT_CRC32:LSZ_INIT_CRC16;
	long crc = 0;
	int frametype = LSZ_ERROR;
	int rcvdata = 1;
	int res;

	while(OK == (res = HASDATA(timeout))) {
		timeout -= (time(NULL) - t); t = time(NULL);
		if((c = ls_readzdle(0)) < 0) return c;
		if(rcvdata) {
			switch(c) {
			case LSZ_CRCE:
			case LSZ_CRCG:
			case LSZ_CRCQ:
			case LSZ_CRCW:
				rcvdata = 0;
				frametype = c & (~0x0100);
				break;
			default:
				*data++ = c; got++;
				crc = crc32?LSZ_UPDATE_CRC32(c,crc):LSZ_UPDATE_CRC16(c,crc);
				break;
			}
		} else {
			incrc <<= 8; incrc |= c &0xFF;
			if(++crcgot == crcl) {
				if(2 == crcl) { incrc = LSZ_FINISH_CRC16(incrc); crc = STOH(crc & 0xFFFF); }
				else { incrc = LSZ_FINISH_CRC32(incrc); crc = LTOH(crc); }
				if (incrc != crc) return LSZ_BADCRC;
				*len = got;
				return frametype;
			}
		}
	}
	return res;
}


/* Send one char with escaping */
void ls_sendchar(int c) 
{
	int esc = 0;
	c &= 0xFF;
	if (ls_Protocol & LSZ_OPTDIRZAP) {	/* We are Direct ZedZap -- escape only <DLE> */
		esc = (ZDLE == c);
	} else {			/* We are normal ZModem */
		if ((ls_Protocol & LSZ_OPTESCAPEALL) && ((c & 0x60) == 0)) { /* Receiver want to escape ALL */
			esc = 1;
		} else {
			switch (c) {
			case XON: case XON | 0x80:
			case XOFF: case XOFF | 0x80: 
			case DLE: case DLE | 0x80:
			case ZDLE:
				esc = 1;
				break;
			default:
				esc = (((ls_txLastSent & 0x7F) == (char)'@') && ((c & 0x7F) == CR));
				break; 
			}
		}
	}
	if (esc) {
		BUFCHAR(ZDLE);
		c ^= 0x40;
	}
	BUFCHAR(ls_txLastSent = c);
}

/* Send one char as two hex digits */
void ls_sendhex(int i) 
{
	char c = (char)(i & 0xFF);
	BUFCHAR(HEX_DIGITS[(c & 0xF0) >> 4]);
	BUFCHAR(ls_txLastSent = HEX_DIGITS[c & 0xF]);
}

/* Retrun 7bit character, strip XON/XOFF if not DirZap, with timeout */
int ls_read7bit(int timeout)
{
	int c;

	do {
		if((c = GETCHAR(timeout)) < 0) return c;
	} while((0 == (ls_Protocol & LSZ_OPTDIRZAP)) && (XON == c || XOFF == c));

	if (CAN == c) { if (++ls_CANCount == 5) return LSZ_CAN; }
	else { ls_CANCount = 0; }
	return c & 0x7F;
}

/* Read one hex character */
int ls_readhexnibble(int timeout) {
	int c;
	if((c = ls_readcanned(timeout)) < 0) return c;
	if(c >= '0' && c <= '9') {
		return c - '0';
	} else if(c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	} else {
		return 0; /* will be CRC error */
	}
}

/* Read chracter as two hex digit */
int ls_readhex(int timeout)
{
	static int c = 0;
	int c2;
	int t = time(NULL);
	int res;

	if(!ls_GotHexNibble) {
		if((c = ls_readhexnibble(timeout)) < 0) return c;
		timeout -= (time(NULL) - t); t = time(NULL);
		c <<= 4;
	}
	if(OK == (res = HASDATS(timeout))) {
		if((c2 = ls_readhexnibble(0)) < 0) return c2;
        ls_GotHexNibble = 0;
		return c | c2;
	} else {
		ls_GotHexNibble = 1;
		return res;
	}
}

/* Retrun 8bit character, strip <DLE> */
int ls_readzdle(int timeout)
{
	int c;

	if(!ls_GotZDLE) { /* There was no ZDLE in stream, try to read one */
		do {
			if((c = ls_readcanned(timeout)) < 0) return c;

			if(!(ls_Protocol & LSZ_OPTDIRZAP)) { /* Check for unescaped XON/XOFF */
				switch(c) {
				case XON: case XON | 0x80:
				case XOFF: case XOFF | 0x80:
					c = LSZ_XONXOFF;
				}
			}
			if (ZDLE == c) { ls_GotZDLE = 1; }
			else if(LSZ_XONXOFF != c) { return c & 0xFF; }
		} while(LSZ_XONXOFF == c);
	}
	/* We will be here only in case of DLE */
	if(HASDATA(0)) { /* We have data RIGHT NOW! */
		ls_GotZDLE = 0;
		if((c = ls_readcanned(timeout)) < 0) return c;
        switch(c) {
		case ZCRCE: return LSZ_CRCE;
		case ZCRCG: return LSZ_CRCG;
		case ZCRCQ: return LSZ_CRCQ;
		case ZCRCW: return LSZ_CRCW;
		case ZRUB0: return ZDEL;
		case ZRUB1: return ZDEL | 0x80;
		default:
			return (c ^ 0x40) & 0xFF;
        }
	}
}

/* Read one character, check for five CANs */
int ls_readcanned(int timeout)
{
	int c;
	if((c = GETCHAR(timeout)) < 0) return c;
	if (CAN == c) { if (++ls_CANCount == 5) return LSZ_CAN; }
	else { ls_CANCount = 0; }
	return c & 0xFF;
}

/* Store long integer (4 bytes) in buffer, as it must be stored in header */
void ls_storelong(char *buf, long l)
{
	l=LTOI(l);
	buf[LSZ_P0] = (l)&0xFF;
	buf[LSZ_P1] = (l>>8)&0xFF;
	buf[LSZ_P2] = (l>>16)&0xFF;
	buf[LSZ_P3] = (l>>24)&0xFF;
}

/* Fetch long integer (4 bytes) from buffer, as it must be stored in header */
long ls_fetchlong(char *buf)
{
	long l = buf[LSZ_P3];
	l<<=8; l|=buf[LSZ_P2];
	l<<=8; l|=buf[LSZ_P1];
	l<<=8; l|=buf[LSZ_P0];
	return LTOH(l);
}