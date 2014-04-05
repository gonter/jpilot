/*******************************************************************************
 * russian.c
 * A module of J-Pilot http://jpilot.org
 *
 * Copyright (C) 2000 by Gennady Kudelya
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 ******************************************************************************/

/*
        Convert charsets: Palm <-> Unix
                          Palm  :  koi8
                          Unix  :  Win1251
*/

/********************************* Includes ***********************************/
#include "config.h"
#include <stdlib.h>
#include "russian.h"

/********************************* Constants **********************************/
static const unsigned char k2w[256] = {
   0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
   0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
   0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
   0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,
   0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,
   0x28,0x29,0x2a,0x2b,0x2c,0x2d,0x2e,0x2f,
   0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,
   0x38,0x39,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f,
   0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47,
   0x48,0x49,0x4a,0x4b,0x4c,0x4d,0x4e,0x4f,
   0x50,0x51,0x52,0x53,0x54,0x55,0x56,0x57,
   0x58,0x59,0x5a,0x5b,0x5c,0x5d,0x5e,0x5f,
   0x60,0x61,0x62,0x63,0x64,0x65,0x66,0x67,
   0x68,0x69,0x6a,0x6b,0x6c,0x6d,0x6e,0x6f,
   0x70,0x71,0x72,0x73,0x74,0x75,0x76,0x77,
   0x78,0x79,0x7a,0x7b,0x7c,0x7d,0x7e,0x7f,
   0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,
   0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,
   0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,
   0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,
   0xa0,0xa1,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
   0xb3,0xa9,0xaa,0xab,0xac,0xad,0xae,0xaf,
   0xb0,0xb1,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,
   0xa3,0xb9,0xba,0xbb,0xbc,0xbd,0xbe,0xbf,
   0xe1,0xe2,0xf7,0xe7,0xe4,0xe5,0xf6,0xfa,
   0xe9,0xea,0xeb,0xec,0xed,0xee,0xef,0xf0,
   0xf2,0xf3,0xf4,0xf5,0xe6,0xe8,0xe3,0xfe,
   0xfb,0xfd,0xff,0xf9,0xf8,0xfc,0xe0,0xf1,
   0xc1,0xc2,0xd7,0xc7,0xc4,0xc5,0xd6,0xda,
   0xc9,0xca,0xcb,0xcc,0xcd,0xce,0xcf,0xd0,
   0xd2,0xd3,0xd4,0xd5,0xc6,0xc8,0xc3,0xde,
   0xdb,0xdd,0xdf,0xd9,0xd8,0xdc,0xc0,0xd1
};

static const unsigned char w2k[256] = {
   0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
   0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
   0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
   0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,
   0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,
   0x28,0x29,0x2a,0x2b,0x2c,0x2d,0x2e,0x2f,
   0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,
   0x38,0x39,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f,
   0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47,
   0x48,0x49,0x4a,0x4b,0x4c,0x4d,0x4e,0x4f,
   0x50,0x51,0x52,0x53,0x54,0x55,0x56,0x57,
   0x58,0x59,0x5a,0x5b,0x5c,0x5d,0x5e,0x5f,
   0x60,0x61,0x62,0x63,0x64,0x65,0x66,0x67,
   0x68,0x69,0x6a,0x6b,0x6c,0x6d,0x6e,0x6f,
   0x70,0x71,0x72,0x73,0x74,0x75,0x76,0x77,
   0x78,0x79,0x7a,0x7b,0x7c,0x7d,0x7e,0x7f,
   0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
   0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
   0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
   0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,
   0xa0,0xa1,0xa2,0xb8,0xa4,0xa5,0xa6,0xa7,
   0xa8,0xa9,0xaa,0xab,0xac,0xad,0xae,0xaf,
   0xb0,0xb1,0xb2,0xa8,0xb4,0xb5,0xb6,0xb7,
   0xb8,0xb9,0xba,0xbb,0xbc,0xbd,0xbe,0xbf,
   0xfe,0xe0,0xe1,0xf6,0xe4,0xe5,0xf4,0xe3,
   0xf5,0xe8,0xe9,0xea,0xeb,0xec,0xed,0xee,
   0xef,0xff,0xf0,0xf1,0xf2,0xf3,0xe6,0xe2,
   0xfc,0xfb,0xe7,0xf8,0xfd,0xf9,0xf7,0xfa,
   0xde,0xc0,0xc1,0xd6,0xc4,0xc5,0xd4,0xc3,
   0xd5,0xc8,0xc9,0xca,0xcb,0xcc,0xcd,0xce,
   0xcf,0xdf,0xd0,0xd1,0xd2,0xd3,0xc6,0xc2,
   0xdc,0xdb,0xc7,0xd8,0xdd,0xd9,0xd7,0xda
};

/****************************** Main Code *************************************/
void win1251_to_koi8(char *const buf, int buf_len)
{
   unsigned char *p;
   int i;

   if (buf == NULL) return;

   for (i=0, p = (unsigned char *)buf; *p && i < buf_len; p++, i++) {
      *p = w2k[(*p)];
   }
}

void koi8_to_win1251(char *const buf, int buf_len)
{
   unsigned char *p;
   int i;

   if (buf == NULL) return;

   for (i=0, p = (unsigned char *)buf; *p && i < buf_len; p++, i++) {
        *p = k2w[(*p)];
   }
}

