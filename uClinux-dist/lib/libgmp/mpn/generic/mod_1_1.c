/* mpn_mod_1s_1p (ap, n, b, cps)
   Divide (ap,,n) by b.  Return the single-limb remainder.
   Requires that b < B / 2.

   Contributed to the GNU project by Torbjorn Granlund.

   THE FUNCTIONS IN THIS FILE ARE INTERNAL WITH MUTABLE INTERFACES.  IT IS ONLY
   SAFE TO REACH THEM THROUGH DOCUMENTED INTERFACES.  IN FACT, IT IS ALMOST
   GUARANTEED THAT THEY WILL CHANGE OR DISAPPEAR IN A FUTURE GNU MP RELEASE.

Copyright 2008, 2009 Free Software Foundation, Inc.

This file is part of the GNU MP Library.

The GNU MP Library is free software; you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation; either version 3 of the License, or (at your
option) any later version.

The GNU MP Library is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
License for more details.

You should have received a copy of the GNU Lesser General Public License
along with the GNU MP Library.  If not, see http://www.gnu.org/licenses/.  */

#include "gmp.h"
#include "gmp-impl.h"
#include "longlong.h"

void
mpn_mod_1s_1p_cps (mp_limb_t cps[4], mp_limb_t b)
{
  mp_limb_t bi;
  mp_limb_t B1modb, B2modb;
  int cnt;

  ASSERT (b <= GMP_NUMB_MAX / 2);

  count_leading_zeros (cnt, b);

  b <<= cnt;
  invert_limb (bi, b);

  B1modb = -b * ((bi >> (GMP_LIMB_BITS-cnt)) | (CNST_LIMB(1) << cnt));
  ASSERT (B1modb <= b);		/* NB: not fully reduced mod b */
  udiv_rnd_preinv (B2modb, B1modb, b, bi);

  B1modb >>= cnt;
  B2modb >>= cnt;

  cps[0] = bi;
  cps[1] = cnt;
  cps[2] = B1modb;
  cps[3] = B2modb;
}

mp_limb_t
mpn_mod_1s_1p (mp_srcptr ap, mp_size_t n, mp_limb_t b, mp_limb_t bmodb[4])
{
  mp_limb_t rh, rl, bi, q, ph, pl, r;
  mp_limb_t B1modb, B2modb;
  mp_size_t i;
  int cnt;

  B1modb = bmodb[2];
  B2modb = bmodb[3];

  umul_ppmm (ph, pl, ap[n - 1], B1modb);
  add_ssaaaa (rh, rl, ph, pl, 0, ap[n - 2]);

  for (i = n - 3; i >= 0; i -= 1)
    {
      /* rr = ap[i]				< B
	    + LO(rr)  * (B mod b)		<= (B-1)(b-1)
	    + HI(rr)  * (B^2 mod b)		<= (B-1)(b-1)
      */
      umul_ppmm (ph, pl, rl, B1modb);
      add_ssaaaa (ph, pl, ph, pl, 0, ap[i]);

      umul_ppmm (rh, rl, rh, B2modb);
      add_ssaaaa (rh, rl, rh, rl, ph, pl);
    }

  bi = bmodb[0];
  cnt = bmodb[1];
#if 1
  {
    mp_limb_t mask;
    r = (rh << cnt) | (rl >> (GMP_LIMB_BITS - cnt));
    mask = -(mp_limb_t) (r >= b);
    r -= mask & b;
  }
#else
  udiv_qrnnd_preinv (q, r, rh >> (GMP_LIMB_BITS - cnt),
		     (rh << cnt) | (rl >> (GMP_LIMB_BITS - cnt)), b, bi);
  ASSERT (q <= 1);	/* optimize for small quotient? */
#endif

  udiv_qrnnd_preinv (q, r, r, rl << cnt, b, bi);

  return r >> cnt;
}
