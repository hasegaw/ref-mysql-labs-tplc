/* Copyright (c) 2001, 2014, Oracle and/or its affiliates. All rights reserved.

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
  Optimized functions for the x86 architecture (_WIN32 included).
*/
static inline int16  sint2korr(const uchar *A) { return *((int16*) A); }

static inline int32 sint3korr(const uchar *A)
{
  return
    ((int32) ((((uchar) (A)[2]) & 128) ?
              (((uint32) 255L << 24) |
               (((uint32) (uchar) (A)[2]) << 16) |
               (((uint32) (uchar) (A)[1]) << 8) |
               ((uint32) (uchar) (A)[0])) :
              (((uint32) (uchar) (A)[2]) << 16) |
              (((uint32) (uchar) (A)[1]) << 8) |
              ((uint32) (uchar) (A)[0])))
    ;
}

static inline int32 sint4korr(const uchar *A) { return *((int32*) A); }

static inline uint16 uint2korr(const uchar *A) { return *((uint16*) A); }

static inline uint32 uint3korr(const uchar *A)
{
  return
    (uint32) (((uint32) ((uchar) (A)[0])) +
              (((uint32) ((uchar) (A)[1])) << 8) +
              (((uint32) ((uchar) (A)[2])) << 16))
    ;
}

static inline uint32 uint4korr(const uchar *A) { return *((uint32*) A); }

static inline ulonglong uint5korr(const uchar *A)
{
  return
    ((ulonglong)(((uint32) ((uchar) (A)[0])) +
                 (((uint32) ((uchar) (A)[1])) << 8) +
                 (((uint32) ((uchar) (A)[2])) << 16) +
                 (((uint32) ((uchar) (A)[3])) << 24)) +
     (((ulonglong) ((uchar) (A)[4])) << 32))
    ;
}

static inline ulonglong uint6korr(const uchar *A)
{
  return
    ((ulonglong)(((uint32)    ((uchar) (A)[0]))          +
                 (((uint32)    ((uchar) (A)[1])) << 8)   +
                 (((uint32)    ((uchar) (A)[2])) << 16)  +
                 (((uint32)    ((uchar) (A)[3])) << 24)) +
     (((ulonglong) ((uchar) (A)[4])) << 32) +
     (((ulonglong) ((uchar) (A)[5])) << 40))
    ;
}
static inline ulonglong uint8korr(const uchar *A) { return *((ulonglong*) A);}
static inline longlong  sint8korr(const uchar *A) { return *((longlong*) A); }

static inline void int2store(uchar *pT, uint16 A)
{
  *((uint16*) pT)= A;
}

static inline void int3store(uchar *T, uint A)
{
  *(T)=  (uchar) ((A));
  *(T+1)=(uchar) (((uint) (A) >> 8));
  *(T+2)=(uchar) (((A) >> 16));
}
static inline void int4store(uchar *pT, uint32 A)
{
  *((uint32*) pT)= A;
}

static inline void int5store(uchar *T, ulonglong A)
{
  *(T)= (uchar)((A));
  *((T)+1)=(uchar) (((A) >> 8));
  *((T)+2)=(uchar) (((A) >> 16));
  *((T)+3)=(uchar) (((A) >> 24));
  *((T)+4)=(uchar) (((A) >> 32));
}
static inline void int6store(uchar *T, ulonglong A)
{
  *(T)=    (uchar)((A));
  *((T)+1)=(uchar) (((A) >> 8));
  *((T)+2)=(uchar) (((A) >> 16));
  *((T)+3)=(uchar) (((A) >> 24));
  *((T)+4)=(uchar) (((A) >> 32));
  *((T)+5)=(uchar) (((A) >> 40));
}
static inline void int8store(uchar *pT, ulonglong A)
{
  *((ulonglong*) pT)= A;
}
