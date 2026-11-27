/* Correctly-rounded arctangent function of two binary32 values.

Copyright (c) 2022-2026 Alexei Sibidanov <sibid@uvic.ca> and Paul Zimmermann.

This file is part of the CORE-MATH project
(https://core-math.gitlabpages.inria.fr/).

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

/* proven_math (RFC-0056): ported from CORE-MATH 03948b7170fe, src/binary32/atan2/atan2f.c. Changed: the
 * headers of fenv, errno, stdio, stdlib, math, inttypes and x86intrin are not included
 * (cm_shim.h: rounding to nearest, no flags); x86-64 MXCSR and intrinsic branches take the
 * portable one; unreachable exits return the integer path's result; the sqrt and nanf
 * builtins are float_ops.h's functions and round, fmax, fmin cm_shim.h's inline ones (the
 * same bits, no libm); cr_NAME is
 * proven_cm_NAME_; FENV_ACCESS left out on wasm; non-ASCII letters in comments
 * transliterated. Nothing else. */

#include "cm_shim.h"
#if PROVEN_CM


#include <stdint.h>
#ifdef CORE_MATH_SUPPORT_ERRNO

#endif

// Warning: clang also defines __GNUC__
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#endif

#if !defined(__wasm__) /* proven_math: no floating-point environment there */
#pragma STDC FENV_ACCESS ON
#endif
typedef union {float f; uint32_t u;} b32u32_u;
typedef union {double f; uint64_t u;} b64u64_u;

static inline double fasttwosum(double x, double y, double *e){
  double s = x + y, z = s - x;
  *e = y - z;
  return s;
}

static inline double fastsum(double xh, double xl, double yh, double yl, double *e){
  double sl, sh = fasttwosum(xh, yh, &sl);
  *e = (xl + yl) + sl;
  return sh;
}

static inline double muldd(double xh, double xl, double ch, double cl, double *l){
  double ahhh = ch*xh;
  *l = (ch*xl + cl*xh) + proven_cm_fma(ch, xh, -ahhh);
  return ahhh;
}

static double polydd(double xh, double xl, int n, const double c[][2], double *l){
  int i = n-1;
  double ch = c[i][0], cl = c[i][1];
  while(--i>=0){
    ch = muldd(xh,xl,ch,cl,&cl);
    ch = fastsum(c[i][0],c[i][1],ch,cl,&cl);
  }
  *l = cl;
  return ch;
}

/* for y/x tiny, use Taylor approximation z - z^3/3 where z=y/x */
static float
proven_cm_atan2f_tiny_ (float y, float x)
{
  double dy = y, dx = x;
  double z = dy / dx;
  double e = proven_cm_fma (-z, x, y);
  /* z * x + e = y thus y/x = z + e/x */
  static const double c = -0x1.5555555555555p-2; /* -1/3 rounded to nearest */
  double zz = z * z;
  double cz = c * z;
  e = e / dx + cz * zz;
  b64u64_u t = {.f = z};
  if ((t.u & 0xfffffffull) == 0) /* boundary case */
  {
    /* If z and e are of same sign (resp. of different signs), we increase
       (resp. decrease) the significant of t by 1 to avoid a double-rounding
       issue when rounding t.f to binary32. */
    if (z * e > 0)
      t.u += 1;
    else
      t.u -= 1;
  }
  float res = t.f;
#ifdef CORE_MATH_SUPPORT_ERRNO
  if (__builtin_fabsf (res) < 0x1p-126f)
    errno = ERANGE; // underflow
#endif
  return res;
}

float proven_cm_atan2f_(float y, float x){
  static const double cn[] =
    {0x1p+0, 0x1.40e0698f94c35p+1, 0x1.248c5da347f0dp+1, 0x1.d873386572976p-1, 0x1.46fa40b20f1dp-3,
     0x1.33f5e041eed0fp-7, 0x1.546bbf28667c5p-14};
  static const double cd[] =
    {0x1p+0, 0x1.6b8b143a3f6dap+1, 0x1.8421201d18ed5p+1, 0x1.8221d086914ebp+0, 0x1.670657e3a07bap-2,
     0x1.0f4951fd1e72dp-5, 0x1.b3874b8798286p-11};
  static const double m[] = {0, 1};
#define pi 0x1.921fb54442d18p+1
#define pi2 0x1.921fb54442d18p+0
#define pi2l 0x1.1a62633145c07p-54
  static const double off[] = {0.0f, pi2, pi, pi2, -0.0f, -pi2, -pi, -pi2};
  static const double offl[] = {0.0f, pi2l, 2*pi2l, pi2l, -0.0f, -pi2l, -2*pi2l, -pi2l};
  static const double sgn[] = {1,-1};
  b32u32_u tx = {.f = x}, ty = {.f = y};
  uint32_t ux = tx.u, uy = ty.u, ax = ux&(~0u>>1), ay = uy&(~0u>>1);
  if(__builtin_expect(ay >= (0xff<<23)||ax >= (0xff<<23), 0)){ // x or y is nan or inf
    /* we use x+y below so that the invalid exception is set
       for (x,y) = (qnan,snan) or (snan,qnan) */
    if(ay > (0xff<<23)) return x + y; // case y nan
    if(ax > (0xff<<23)) return x + y; // case x nan
    uint32_t yinf = ay==(0xff<<23), xinf = ax==(0xff<<23);
    if(yinf&xinf){
      if(ux>>31)
	return 0x1.2d97c7f3321d2p+1*sgn[uy>>31]; // +/-3pi/4
      else
	return 0x1.921fb54442d18p-1*sgn[uy>>31]; // +/-pi/4
    }
    if(xinf){
      if(ux>>31)
	return pi*sgn[uy>>31];
      else
	return 0.0*sgn[uy>>31];
    }
    if(yinf){
      return pi2*sgn[uy>>31];
    }
  }
  if(__builtin_expect(ay==0, 0)){
    if(__builtin_expect(!ax,0)){
      uint32_t i = (uy>>31)*4 + (ux>>31)*2;
      if(ux>>31)
	return off[i] + offl[i];
      else
	return off[i];
    }
    if(!(ux>>31)) return 0.0*sgn[uy>>31];
  }
  uint32_t gt = ay>ax, i = (uy>>31)*4 + (ux>>31)*2 + gt;

  double zx = x, zy = y;
  double z = (m[gt]*zx + m[1-gt]*zy)/(m[gt]*zy + m[1-gt]*zx);
  // z = x/y if |y| > |x|, and z = y/x otherwise
  double r;
  int d = (int)ax-(int)ay;
  if (__builtin_expect(d<(27<<23)&&d>(-(27<<23)),1)){
    double z2 = z*z, z4 = z2*z2, z8 = z4*z4;
    /* z2 cannot underflow, since for |y|=0x1p-149 and |x|=0x1.fffffep+127
       we get |z| > 2^-277 thus z2 > 2^-554, but z4 and z8 might underflow,
       which might give spurious underflow exceptions. */
    double cn0 = cn[0] + z2*cn[1];
    double cn2 = cn[2] + z2*cn[3];
    double cn4 = cn[4] + z2*cn[5];
    double cn6 = cn[6];
    cn0 += z4*cn2;
    cn4 += z4*cn6;
    cn0 += z8*cn4;
    double cd0 = cd[0] + z2*cd[1];
    double cd2 = cd[2] + z2*cd[3];
    double cd4 = cd[4] + z2*cd[5];
    double cd6 = cd[6];
    cd0 += z4*cd2;
    cd4 += z4*cd6;
    cd0 += z8*cd4;
    r = cn0/cd0;
  } else {
    r = 1;
  }
  z *= sgn[gt];
  r = z*r + off[i];
  b64u64_u res = {.f = r};
  if(__builtin_expect(((res.u + 8)&0xfffffff) <= 16, 0)){
    /* check tiny y/x */
    if (ay < ax && ((ax - ay) >> 23 >= 25))
      return proven_cm_atan2f_tiny_ (y, x);
    double zh,zl;
    if(!gt){
      zh = zy/zx;
      zl = proven_cm_fma(zh,-zx,zy)/zx;
    } else {
      zh = zx/zy;
      zl = proven_cm_fma(zh,-zy,zx)/zy;
    }
    double z2l, z2h = muldd(zh,zl,zh,zl,&z2l);
    static const double c[][2] = {
      {0x1p+0, -0x1.f54b39202367p-108}, {-0x1.5555555555555p-2, -0x1.5555555553c5bp-56},
      {0x1.999999999999ap-3, -0x1.999999a03b7b1p-57}, {-0x1.2492492492492p-3, -0x1.249243845da75p-57},
      {0x1.c71c71c71c71cp-4, 0x1.c71758a0994cfp-58}, {-0x1.745d1745d1746p-4, 0x1.773a136088f0cp-59},
      {0x1.3b13b13b13b14p-4, -0x1.80e1a68002b5ap-58}, {-0x1.111111111110fp-4, 0x1.509ec760b1712p-58},
      {0x1.e1e1e1e1e1d9fp-5, 0x1.ac329985f21c6p-59}, {-0x1.af286bca1a4fbp-5, -0x1.733f50c077ddap-59},
      {0x1.861861860df4bp-5, -0x1.e3d7be4c4f7efp-59}, {-0x1.642c8590271acp-5, -0x1.f507c4cd57f3dp-59},
      {0x1.47ae1474e5e95p-5, -0x1.2a2236f400b31p-63}, {-0x1.2f684ba2df0d2p-5, 0x1.c8917e625652ep-60},
      {0x1.1a7b9462444aep-5, 0x1.9f014770ba99ap-59}, {-0x1.08420542dfdf2p-5, -0x1.269b5b87ea3c6p-60},
      {0x1.f07b9d85e9e7ap-6, -0x1.382cb6d9e62aap-63}, {-0x1.d41abbafee9bcp-6, -0x1.e34acb821089ap-60},
      {0x1.bac495a191284p-6, 0x1.71c573cb85acp-60}, {-0x1.a3f05da330b5cp-6, 0x1.89f376cc690d7p-60},
      {0x1.8f0f71dd71f45p-6, 0x1.d9ec721225064p-61}, {-0x1.7b649e65046fbp-6, -0x1.8fdde9c5935eap-61},
      {0x1.67ccaafe6a94ep-6, 0x1.01ecd1b84b532p-61}, {-0x1.528df69730b57p-6, -0x1.d8251501b2b7p-60},
      {0x1.396309aafbd36p-6, 0x1.8bbcce2773e0cp-60}, {-0x1.19fe2fc6ddc6ap-6, -0x1.c40d6f4eeee8bp-61},
      {0x1.e624313891fc1p-7, 0x1.20cc7590c1a1ep-64}, {-0x1.8af3578592d2fp-7, 0x1.de3b15c37f067p-64},
      {0x1.296bd710c7f77p-7, 0x1.5c3e4434f420fp-61}, {-0x1.98784ba29866cp-8, 0x1.3a6200b2f6dc8p-62},
      {0x1.f768a067c89a6p-9, -0x1.ac1c06c977a74p-64}, {-0x1.11f0c2a247746p-9, 0x1.d551cfc3d45bdp-65},
      {0x1.02e03a233033dp-10, -0x1.23564e0d06dbp-69}, {-0x1.a0dc69bc309bfp-12, 0x1.f756430f06439p-72},
      {0x1.177dfa90ec884p-13, -0x1.db3bfc5776783p-67}, {-0x1.2ef944b87e2d2p-15, 0x1.19318d105a999p-71},
      {0x1.fd49cf4251e6ep-18, -0x1.3bf24affec5adp-72}, {-0x1.36fab0b722d3ap-20, 0x1.f3a497841e34ap-75},
      {0x1.ea66b8828c9d9p-24, -0x1.7fefc41e5a088p-79}, {-0x1.767ab6da14ccfp-28, 0x1.418403c6477f4p-84}};
    double pl, ph = polydd(z2h, z2l, 40, c, &pl);
    zh *= sgn[gt];
    zl *= sgn[gt];
    ph = muldd(zh,zl,ph,pl,&pl);
    double sh = ph + off[i], sl = ((off[i] - sh) + ph) + pl + offl[i];
    sh = fasttwosum(sh,sl, &sl);
    b64u64_u th = {.f = sh};
    if(sh + sh*0x1p-60 == sh - sh*0x1p-60){
      if(sh*sl<0) th.u-=!(th.u&0xfffffff);
      if(sh*sl>0) th.u|=1;
    }
    r = th.f;
  }
  float rf = r;
#ifdef CORE_MATH_SUPPORT_ERRNO
  // if rf underflows, set errno=ERANGE
  if (__builtin_expect (__builtin_fabsf (rf) < 0x1p-126f, 0))
    errno = ERANGE; // underflow
#endif
  return rf;
}

#include "cm_shim_end.h"
#endif /* PROVEN_CM */
