/* FreeRDP: A Remote Desktop Protocol Client
 * Optimized YCoCg<->RGB conversion operations.
 * vi:ts=4 sw=4:
 *
 * (c) Copyright 2014 Hewlett-Packard Development Company, L.P.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <freerdp/config.h>

#include <freerdp/types.h>
#include <freerdp/primitives.h>
#include <winpr/sysinfo.h>

#include "prim_YCoCg.h"

#include "prim_internal.h"
#include "prim_templates.h"

#if defined(SSE_AVX_INTRINSICS_ENABLED)
#include <emmintrin.h>
#include <tmmintrin.h>

static primitives_t* generic = NULL;

/* ------------------------------------------------------------------------- */
static pstatus_t ssse3_YCoCgRToRGB_8u_AC4R_invert(const BYTE* WINPR_RESTRICT pSrc, UINT32 srcStep,
                                                  BYTE* WINPR_RESTRICT pDst, UINT32 DstFormat,
                                                  UINT32 dstStep, UINT32 width, UINT32 height,
                                                  UINT8 shift, BOOL withAlpha)
{
	const BYTE* sptr = pSrc;
	BYTE* dptr = pDst;

	WINPR_ASSERT(srcStep / sizeof(UINT32) >= width);
	WINPR_ASSERT(dstStep / sizeof(UINT32) >= width);
	const size_t sRowBump = srcStep - width * sizeof(UINT32);
	const size_t dRowBump = dstStep - width * sizeof(UINT32);
	/* Shift left by "shift" and divide by two is the same as shift
	 * left by "shift-1".
	 */
	int dataShift = shift - 1;
	BYTE mask = (BYTE)(0xFFU << dataShift);

	/* Let's say the data is of the form:
	 * y0y0o0g0 a1y1o1g1 a2y2o2g2...
	 * Apply:
	 * |R|   | 1  1/2 -1/2 |   |y|
	 * |G| = | 1  0    1/2 | * |o|
	 * |B|   | 1 -1/2 -1/2 |   |g|
	 * where Y is 8-bit unsigned and o & g are 8-bit signed.
	 */

	if ((width < 8) || (ULONG_PTR)dptr & 0x03)
	{
		/* Too small, or we'll never hit a 16-byte boundary.  Punt. */
		return generic->YCoCgToRGB_8u_AC4R(pSrc, WINPR_ASSERTING_INT_CAST(INT32, srcStep), pDst,
		                                   DstFormat, WINPR_ASSERTING_INT_CAST(INT32, dstStep),
		                                   width, height, shift, withAlpha);
	}

	for (UINT32 h = 0; h < height; h++)
	{
		UINT32 w = width;

		while (w >= 8)
		{
			__m128i R0;
			__m128i R1;
			__m128i R2;
			__m128i R3;
			__m128i R4;
			__m128i R5;
			__m128i R6;
			__m128i R7;

			R0 = LOAD_SI128(sptr);
			sptr += (128 / 8);
			R1 = LOAD_SI128(sptr);
			sptr += (128 / 8);

			/* R0 = a3y3o3g3 a2y2o2g2 a1y1o1g1 a0y0o0g0 */
			/* R1 = a7y7o7g7 a6y6o6g6 a5y5o5g5 a4y4o4g4 */
			/* Shuffle to pack all the like types together. */
			R2 = _mm_set_epi32(0x0f0b0703, 0x0e0a0602, 0x0d090501, 0x0c080400);
			R3 = _mm_shuffle_epi8(R0, R2);
			R4 = _mm_shuffle_epi8(R1, R2);
			/* R3 = a3a2a1a0 y3y2y1y0 o3o2o1o0 g3g2g1g0 */
			/* R4 = a7a6a5a4 y7y6y5y4 o7o6o5o4 g7g6g5g4 */
			R5 = _mm_unpackhi_epi32(R3, R4);
			R6 = _mm_unpacklo_epi32(R3, R4);

			/* R5 = a7a6a5a4 a3a2a1a0 y7y6y5y4 y3y2y1y0 */
			/* R6 = o7o6o5o4 o3o2o1o0 g7g6g5g4 g3g2g1g0 */
			/* Save alphas aside */
			if (withAlpha)
				R7 = _mm_unpackhi_epi64(R5, R5);
			else
				R7 = mm_set1_epu32(0xFFFFFFFFU);

			/* R7 = a7a6a5a4 a3a2a1a0 a7a6a5a4 a3a2a1a0 */
			/* Expand Y's from 8-bit unsigned to 16-bit signed. */
			R1 = mm_set1_epu32(0);
			R0 = _mm_unpacklo_epi8(R5, R1);
			/* R0 = 00y700y6 00y500y4 00y300y2 00y100y0 */
			/* Shift Co's and Cg's by (shift-1).  -1 covers division by two.
			 * Note: this must be done before sign-conversion.
			 * Note also there is no slli_epi8, so we have to use a 16-bit
			 * version and then mask.
			 */
			R6 = _mm_slli_epi16(R6, dataShift);
			R1 = mm_set1_epu8(mask);
			R6 = _mm_and_si128(R6, R1);
			/* R6 = shifted o7o6o5o4 o3o2o1o0 g7g6g5g4 g3g2g1g0 */
			/* Expand Co's from 8-bit signed to 16-bit signed */
			R1 = _mm_unpackhi_epi8(R6, R6);
			R1 = _mm_srai_epi16(R1, 8);
			/* R1 = xxo7xxo6 xxo5xxo4 xxo3xxo2 xxo1xxo0 */
			/* Expand Cg's form 8-bit signed to 16-bit signed */
			R2 = _mm_unpacklo_epi8(R6, R6);
			R2 = _mm_srai_epi16(R2, 8);
			/* R2 = xxg7xxg6 xxg5xxg4 xxg3xxg2 xxg1xxg0 */
			/* Get Y - halfCg and save */
			R6 = _mm_subs_epi16(R0, R2);
			/* R = (Y-halfCg) + halfCo */
			R3 = _mm_adds_epi16(R6, R1);
			/* R3 = xxR7xxR6 xxR5xxR4 xxR3xxR2 xxR1xxR0 */
			/* G = Y + Cg(/2) */
			R4 = _mm_adds_epi16(R0, R2);
			/* R4 = xxG7xxG6 xxG5xxG4 xxG3xxG2 xxG1xxG0 */
			/* B = (Y-halfCg) - Co(/2) */
			R5 = _mm_subs_epi16(R6, R1);
			/* R5 = xxB7xxB6 xxB5xxB4 xxB3xxB2 xxB1xxB0 */
			/* Repack R's & B's.  */
			R0 = _mm_packus_epi16(R3, R5);
			/* R0 = R7R6R5R4 R3R2R1R0 B7B6B5B4 B3B2B1B0 */
			/* Repack G's. */
			R1 = _mm_packus_epi16(R4, R4);
			/* R1 = G7G6G6G4 G3G2G1G0 G7G6G6G4 G3G2G1G0 */
			/* And add the A's. */
			R1 = _mm_unpackhi_epi64(R1, R7);
			/* R1 = A7A6A6A4 A3A2A1A0 G7G6G6G4 G3G2G1G0 */
			/* Now do interleaving again. */
			R2 = _mm_unpacklo_epi8(R0, R1);
			/* R2 = G7B7G6B6 G5B5G4B4 G3B3G2B2 G1B1G0B0 */
			R3 = _mm_unpackhi_epi8(R0, R1);
			/* R3 = A7R7A6R6 A5R5A4R4 A3R3A2R2 A1R1A0R0 */
			R4 = _mm_unpacklo_epi16(R2, R3);
			/* R4 = A3R3G3B3 A2R2G2B2 A1R1G1B1 A0R0G0B0 */
			R5 = _mm_unpackhi_epi16(R2, R3);
			/* R5 = A7R7G7B7 A6R6G6B6 A5R6G5B5 A4R4G4B4 */
			STORE_SI128(dptr, R4);
			dptr += (128 / 8);
			STORE_SI128(dptr, R5);
			dptr += (128 / 8);
			w -= 8;
		}

		/* Handle any remainder pixels. */
		if (w > 0)
		{
			pstatus_t status = 0;
			status = generic->YCoCgToRGB_8u_AC4R(
			    sptr, WINPR_ASSERTING_INT_CAST(INT32, srcStep), dptr, DstFormat,
			    WINPR_ASSERTING_INT_CAST(INT32, dstStep), w, 1, shift, withAlpha);

			if (status != PRIMITIVES_SUCCESS)
				return status;

			sptr += w * sizeof(UINT32);
			dptr += w * sizeof(UINT32);
		}

		sptr += sRowBump;
		dptr += dRowBump;
	}

	return PRIMITIVES_SUCCESS;
}

/* ------------------------------------------------------------------------- */
static pstatus_t ssse3_YCoCgRToRGB_8u_AC4R_no_invert(const BYTE* WINPR_RESTRICT pSrc,
                                                     UINT32 srcStep, BYTE* WINPR_RESTRICT pDst,
                                                     UINT32 DstFormat, UINT32 dstStep, UINT32 width,
                                                     UINT32 height, UINT8 shift, BOOL withAlpha)
{
	const BYTE* sptr = pSrc;
	BYTE* dptr = pDst;
	size_t sRowBump = srcStep - width * sizeof(UINT32);
	size_t dRowBump = dstStep - width * sizeof(UINT32);
	/* Shift left by "shift" and divide by two is the same as shift
	 * left by "shift-1".
	 */
	int dataShift = shift - 1;
	BYTE mask = (BYTE)(0xFFU << dataShift);

	/* Let's say the data is of the form:
	 * y0y0o0g0 a1y1o1g1 a2y2o2g2...
	 * Apply:
	 * |R|   | 1  1/2 -1/2 |   |y|
	 * |G| = | 1  0    1/2 | * |o|
	 * |B|   | 1 -1/2 -1/2 |   |g|
	 * where Y is 8-bit unsigned and o & g are 8-bit signed.
	 */

	if ((width < 8) || (ULONG_PTR)dptr & 0x03)
	{
		/* Too small, or we'll never hit a 16-byte boundary.  Punt. */
		return generic->YCoCgToRGB_8u_AC4R(pSrc, WINPR_ASSERTING_INT_CAST(INT32, srcStep), pDst,
		                                   DstFormat, WINPR_ASSERTING_INT_CAST(INT32, dstStep),
		                                   width, height, shift, withAlpha);
	}

	for (UINT32 h = 0; h < height; h++)
	{
		UINT32 w = width;

		while (w >= 8)
		{
			__m128i R7;

			/* The faster path, 16-byte aligned load. */
			__m128i R0 = LOAD_SI128(sptr);
			sptr += (128 / 8);
			__m128i R1 = LOAD_SI128(sptr);
			sptr += (128 / 8);

			/* R0 = a3y3o3g3 a2y2o2g2 a1y1o1g1 a0y0o0g0 */
			/* R1 = a7y7o7g7 a6y6o6g6 a5y5o5g5 a4y4o4g4 */
			/* Shuffle to pack all the like types together. */
			__m128i R2 = _mm_set_epi32(0x0f0b0703, 0x0e0a0602, 0x0d090501, 0x0c080400);
			__m128i R3 = _mm_shuffle_epi8(R0, R2);
			__m128i R4 = _mm_shuffle_epi8(R1, R2);
			/* R3 = a3a2a1a0 y3y2y1y0 o3o2o1o0 g3g2g1g0 */
			/* R4 = a7a6a5a4 y7y6y5y4 o7o6o5o4 g7g6g5g4 */
			__m128i R5 = _mm_unpackhi_epi32(R3, R4);
			__m128i R6 = _mm_unpacklo_epi32(R3, R4);

			/* R5 = a7a6a5a4 a3a2a1a0 y7y6y5y4 y3y2y1y0 */
			/* R6 = o7o6o5o4 o3o2o1o0 g7g6g5g4 g3g2g1g0 */
			/* Save alphas aside */
			if (withAlpha)
				R7 = _mm_unpackhi_epi64(R5, R5);
			else
				R7 = mm_set1_epu32(0xFFFFFFFFU);

			/* R7 = a7a6a5a4 a3a2a1a0 a7a6a5a4 a3a2a1a0 */
			/* Expand Y's from 8-bit unsigned to 16-bit signed. */
			R1 = mm_set1_epu32(0);
			R0 = _mm_unpacklo_epi8(R5, R1);
			/* R0 = 00y700y6 00y500y4 00y300y2 00y100y0 */
			/* Shift Co's and Cg's by (shift-1).  -1 covers division by two.
			 * Note: this must be done before sign-conversion.
			 * Note also there is no slli_epi8, so we have to use a 16-bit
			 * version and then mask.
			 */
			R6 = _mm_slli_epi16(R6, dataShift);
			R1 = mm_set1_epu8(mask);
			R6 = _mm_and_si128(R6, R1);
			/* R6 = shifted o7o6o5o4 o3o2o1o0 g7g6g5g4 g3g2g1g0 */
			/* Expand Co's from 8-bit signed to 16-bit signed */
			R1 = _mm_unpackhi_epi8(R6, R6);
			R1 = _mm_srai_epi16(R1, 8);
			/* R1 = xxo7xxo6 xxo5xxo4 xxo3xxo2 xxo1xxo0 */
			/* Expand Cg's form 8-bit signed to 16-bit signed */
			R2 = _mm_unpacklo_epi8(R6, R6);
			R2 = _mm_srai_epi16(R2, 8);
			/* R2 = xxg7xxg6 xxg5xxg4 xxg3xxg2 xxg1xxg0 */
			/* Get Y - halfCg and save */
			R6 = _mm_subs_epi16(R0, R2);
			/* R = (Y-halfCg) + halfCo */
			R3 = _mm_adds_epi16(R6, R1);
			/* R3 = xxR7xxR6 xxR5xxR4 xxR3xxR2 xxR1xxR0 */
			/* G = Y + Cg(/2) */
			R4 = _mm_adds_epi16(R0, R2);
			/* R4 = xxG7xxG6 xxG5xxG4 xxG3xxG2 xxG1xxG0 */
			/* B = (Y-halfCg) - Co(/2) */
			R5 = _mm_subs_epi16(R6, R1);
			/* R5 = xxB7xxB6 xxB5xxB4 xxB3xxB2 xxB1xxB0 */
			/* Repack R's & B's.  */
			/* This line is the only diff between inverted and non-inverted.
			 * Unfortunately, it would be expensive to check "inverted"
			 * every time through this loop.
			 */
			R0 = _mm_packus_epi16(R5, R3);
			/* R0 = B7B6B5B4 B3B2B1B0 R7R6R5R4 R3R2R1R0 */
			/* Repack G's. */
			R1 = _mm_packus_epi16(R4, R4);
			/* R1 = G7G6G6G4 G3G2G1G0 G7G6G6G4 G3G2G1G0 */
			/* And add the A's. */
			R1 = _mm_unpackhi_epi64(R1, R7);
			/* R1 = A7A6A6A4 A3A2A1A0 G7G6G6G4 G3G2G1G0 */
			/* Now do interleaving again. */
			R2 = _mm_unpacklo_epi8(R0, R1);
			/* R2 = G7B7G6B6 G5B5G4B4 G3B3G2B2 G1B1G0B0 */
			R3 = _mm_unpackhi_epi8(R0, R1);
			/* R3 = A7R7A6R6 A5R5A4R4 A3R3A2R2 A1R1A0R0 */
			R4 = _mm_unpacklo_epi16(R2, R3);
			/* R4 = A3R3G3B3 A2R2G2B2 A1R1G1B1 A0R0G0B0 */
			R5 = _mm_unpackhi_epi16(R2, R3);
			/* R5 = A7R7G7B7 A6R6G6B6 A5R6G5B5 A4R4G4B4 */
			STORE_SI128(dptr, R4);
			dptr += (128 / 8);
			STORE_SI128(dptr, R5);
			dptr += (128 / 8);
			w -= 8;
		}

		/* Handle any remainder pixels. */
		if (w > 0)
		{
			pstatus_t status = 0;
			status = generic->YCoCgToRGB_8u_AC4R(
			    sptr, WINPR_ASSERTING_INT_CAST(INT32, srcStep), dptr, DstFormat,
			    WINPR_ASSERTING_INT_CAST(INT32, dstStep), WINPR_ASSERTING_INT_CAST(UINT32, w), 1,
			    shift, withAlpha);

			if (status != PRIMITIVES_SUCCESS)
				return status;

			sptr += WINPR_ASSERTING_INT_CAST(UINT32, w) * sizeof(UINT32);
			dptr += WINPR_ASSERTING_INT_CAST(UINT32, w) * sizeof(UINT32);
		}

		sptr += sRowBump;
		dptr += dRowBump;
	}

	return PRIMITIVES_SUCCESS;
}

/* ------------------------------------------------------------------------- */
static pstatus_t ssse3_YCoCgRToRGB_8u_AC4R(const BYTE* WINPR_RESTRICT pSrc, INT32 srcStep,
                                           BYTE* WINPR_RESTRICT pDst, UINT32 DstFormat,
                                           INT32 dstStep, UINT32 width, UINT32 height, UINT8 shift,
                                           BOOL withAlpha)
{
	switch (DstFormat)
	{
		case PIXEL_FORMAT_BGRX32:
		case PIXEL_FORMAT_BGRA32:
			return ssse3_YCoCgRToRGB_8u_AC4R_invert(
			    pSrc, WINPR_ASSERTING_INT_CAST(UINT32, srcStep), pDst, DstFormat,
			    WINPR_ASSERTING_INT_CAST(UINT32, dstStep), width, height, shift, withAlpha);

		case PIXEL_FORMAT_RGBX32:
		case PIXEL_FORMAT_RGBA32:
			return ssse3_YCoCgRToRGB_8u_AC4R_no_invert(
			    pSrc, WINPR_ASSERTING_INT_CAST(UINT32, srcStep), pDst, DstFormat,
			    WINPR_ASSERTING_INT_CAST(UINT32, dstStep), width, height, shift, withAlpha);

		default:
			return generic->YCoCgToRGB_8u_AC4R(pSrc, srcStep, pDst, DstFormat, dstStep, width,
			                                   height, shift, withAlpha);
	}
}

#endif

/* ------------------------------------------------------------------------- */
void primitives_init_YCoCg_ssse3_int(primitives_t* WINPR_RESTRICT prims)
{
#if defined(SSE_AVX_INTRINSICS_ENABLED)
	generic = primitives_get_generic();

	WLog_VRB(PRIM_TAG, "SSE3/SSSE3 optimizations");
	prims->YCoCgToRGB_8u_AC4R = ssse3_YCoCgRToRGB_8u_AC4R;
#else
	WLog_VRB(PRIM_TAG, "undefined WITH_SIMD or SSE2 intrinsics not available");
	WINPR_UNUSED(prims);
#endif
}
