/* Copyright (c) 2012, 2014, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

/*
  Data in little-endian format.
*/

#include <string.h>

static inline void float4get  (float  *V, const uchar *M) 
{
  memcpy(V, (M), sizeof(float));
}

static inline void float4store(uchar  *V, float  M)
{
  memcpy(V, (&M), sizeof(float));
}

static inline void float8get  (double *V, const uchar *M)
{
  memcpy(V,  M, sizeof(double));
}

static inline void float8store(uchar  *V, double M)
{
  memcpy(V, &M, sizeof(double));
}

static inline void floatget   (float  *V, const uchar *M) { float4get(V, M); }
static inline void floatstore (uchar  *V, float M)        { float4store(V, M); }

/* Bi-endian hardware.... */
#if defined(__FLOAT_WORD_ORDER) && (__FLOAT_WORD_ORDER == __BIG_ENDIAN)
static inline void doublestore(uchar *T, double V)
{ *(((char*)T)+0)=(char) ((uchar *) &V)[4];
  *(((char*)T)+1)=(char) ((uchar *) &V)[5];
  *(((char*)T)+2)=(char) ((uchar *) &V)[6];
  *(((char*)T)+3)=(char) ((uchar *) &V)[7];
  *(((char*)T)+4)=(char) ((uchar *) &V)[0];
  *(((char*)T)+5)=(char) ((uchar *) &V)[1];
  *(((char*)T)+6)=(char) ((uchar *) &V)[2];
  *(((char*)T)+7)=(char) ((uchar *) &V)[3]; }
static inline void doubleget(double *V, const uchar *M)
{ double def_temp;
  ((uchar*) &def_temp)[0]=(M)[4];
  ((uchar*) &def_temp)[1]=(M)[5];
  ((uchar*) &def_temp)[2]=(M)[6];
  ((uchar*) &def_temp)[3]=(M)[7];
  ((uchar*) &def_temp)[4]=(M)[0];
  ((uchar*) &def_temp)[5]=(M)[1];
  ((uchar*) &def_temp)[6]=(M)[2];
  ((uchar*) &def_temp)[7]=(M)[3];
  (*V) = def_temp; }

#else /* Bi-endian hardware.... */

static inline void doublestore(uchar  *T, double V)       { memcpy(T, &V, sizeof(double)); }
static inline void doubleget  (double *V, const uchar *M) { memcpy(V, M, sizeof(double)); }

#endif /* Bi-endian hardware.... */

#define ushortget(V,M)	do { uchar *pM= (uchar*)(M);V = uint2korr(pM);} while(0)
#define shortget(V,M)	do { uchar *pM= (uchar*)(M);V = sint2korr(pM);} while(0)
#define longget(V,M)	do { uchar *pM= (uchar*)(M);V = sint4korr(pM);} while(0)
#define ulongget(V,M)   do { uchar *pM= (uchar*)(M);V = uint4korr(pM);} while(0)
#define shortstore(T,V) int2store(T,V)
#define longstore(T,V)	int4store(T,V)

#define longlongget(V,M) memcpy(&V, (M), sizeof(ulonglong))
#define longlongstore(T,V) memcpy((T), &V, sizeof(ulonglong))
