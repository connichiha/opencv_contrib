/*M///////////////////////////////////////////////////////////////////////////////////////
 //
 //  IMPORTANT: READ BEFORE DOWNLOADING, COPYING, INSTALLING OR USING.
 //
 //  By downloading, copying, installing or using the software you agree to this license.
 //  If you do not agree to this license, do not download, install,
 //  copy or use the software.
 //
 //
 //                           License Agreement
 //                For Open Source Computer Vision Library
 //
 // Copyright (C) 2014, OpenCV Foundation, all rights reserved.
 // Third party copyrights are property of their respective owners.
 //
 // Redistribution and use in source and binary forms, with or without modification,
 // are permitted provided that the following conditions are met:
 //
 //   * Redistribution's of source code must retain the above copyright notice,
 //     this list of conditions and the following disclaimer.
 //
 //   * Redistribution's in binary form must reproduce the above copyright notice,
 //     this list of conditions and the following disclaimer in the documentation
 //     and/or other materials provided with the distribution.
 //
 //   * The name of the copyright holders may not be used to endorse or promote products
 //     derived from this software without specific prior written permission.
 //
 // This software is provided by the copyright holders and contributors "as is" and
 // any express or implied warranties, including, but not limited to, the implied
 // warranties of merchantability and fitness for a particular purpose are disclaimed.
 // In no event shall the Intel Corporation or contributors be liable for any direct,
 // indirect, incidental, special, exemplary, or consequential damages
 // (including, but not limited to, procurement of substitute goods or services;
 // loss of use, data, or profits; or business interruption) however caused
 // and on any theory of liability, whether in contract, strict liability,
 // or tort (including negligence or otherwise) arising in any way out of
 // the use of this software, even if advised of the possibility of such damage.
 //
 //M*/

//#include "BING/saliencyClasses.h"
#include "saliencyClasses.h"
#include "CmShow.hpp"

namespace cv
{
namespace saliency
{

struct TIGbits
{
  TIGbits() : bc0(0), bc1(0) {}
  inline void accumulate(int64_t tig, int64_t tigMask0, int64_t tigMask1, uchar shift)
  {
    bc0 += ((POPCNT64(tigMask0 & tig) << 1) - POPCNT64(tig)) << shift;
    bc1 += ((POPCNT64(tigMask1 & tig) << 1) - POPCNT64(tig)) << shift;
  }
  int64_t bc0;
  int64_t bc1;
};

float ObjectnessBING::FilterTIG::dot( int64_t tig1, int64_t tig2, int64_t tig4, int64_t tig8 )
{
  TIGbits x;
  x.accumulate(tig1, _bTIGs[0], _bTIGs[1], 0);
  x.accumulate(tig2, _bTIGs[0], _bTIGs[1], 1);
  x.accumulate(tig4, _bTIGs[0], _bTIGs[1], 2);
  x.accumulate(tig8, _bTIGs[0], _bTIGs[1], 3);
  return _coeffs1[0] * x.bc0 + _coeffs1[1] * x.bc1;
}

void ObjectnessBING::FilterTIG::update( Mat &w1f )
{
  CV_Assert( w1f.cols * w1f.rows == D && w1f.type() == CV_32F && w1f.isContinuous() );
  float b[D], residuals[D];
  memcpy( residuals, w1f.data, sizeof(float) * D );
  for ( int i = 0; i < NUM_COMP; i++ )
  {
    float avg = 0;
    for ( int j = 0; j < D; j++ )
    {
      b[j] = residuals[j] >= 0.0f ? 1.0f : -1.0f;
      avg += residuals[j] * b[j];
    }
    avg /= D;
    _coeffs1[i] = avg, _coeffs2[i] = avg * 2, _coeffs4[i] = avg * 4, _coeffs8[i] = avg * 8;
    for ( int j = 0; j < D; j++ )
      residuals[j] -= avg * b[j];
    uint64_t tig = 0;
    for ( int j = 0; j < D; j++ )
      tig = ( tig << 1 ) | ( b[j] > 0 ? 1 : 0 );
    _bTIGs[i] = tig;
  }
}

void ObjectnessBING::FilterTIG::reconstruct( Mat &w1f )
{
  w1f = Mat::zeros( 8, 8, CV_32F );
  float *weight = (float*) w1f.data;
  for ( int i = 0; i < NUM_COMP; i++ )
  {
    uint64_t tig = _bTIGs[i];
    for ( int j = 0; j < D; j++ )
      weight[j] += _coeffs1[i] * ( ( ( tig >> ( 63 - j ) ) & 1 ) ? 1 : -1 );
  }
}

// For a W by H gradient magnitude map, find a W-7 by H-7 CV_32F matching score map
// Please refer to my paper for definition of the variables used in this function
void ObjectnessBING::FilterTIG::matchTemplate(const Mat &mag1u, Mat &matchCost1f)
{
	const int H = mag1u.rows, W = mag1u.cols;
	const Size sz(W + 1, H + 1); // Expand original size to avoid dealing with boundary conditions  
	Mat_<float> scores(sz);
	const int sizeSZ = sz.width * sz.height;
	INT64 * Tig1 = (INT64 *)malloc(sizeSZ * sizeof(INT64));
	INT64 * Tig2 = (INT64 *)malloc(sizeSZ * sizeof(INT64));
	INT64 * Tig4 = (INT64 *)malloc(sizeSZ * sizeof(INT64));
	INT64 * Tig8 = (INT64 *)malloc(sizeSZ * sizeof(INT64));
	byte * Row1 = (byte *)malloc(sizeSZ * sizeof(byte));
	byte * Row2 = (byte *)malloc(sizeSZ * sizeof(byte));
	byte * Row4 = (byte *)malloc(sizeSZ * sizeof(byte));
	byte * Row8 = (byte *)malloc(sizeSZ * sizeof(byte));
	memset(Tig1, 0, sizeSZ * sizeof(INT64)); memset(Tig2, 0, sizeSZ * sizeof(INT64));
	memset(Tig4, 0, sizeSZ * sizeof(INT64)); memset(Tig8, 0, sizeSZ * sizeof(INT64));
	memset(Row1, 0, sizeSZ * sizeof(byte)); memset(Row2, 0, sizeSZ * sizeof(byte));
	memset(Row4, 0, sizeSZ * sizeof(byte)); memset(Row8, 0, sizeSZ * sizeof(byte));
	for (int y = 1; y <= H; y++)
	{
		const byte * G = mag1u.ptr(y - 1);
		INT64 * T1 = Tig1 + y*sz.width;
		INT64 * T2 = Tig2 + y*sz.width;
		INT64 * T4 = Tig4 + y*sz.width;
		INT64 * T8 = Tig8 + y*sz.width;
		INT64 * Tu1 = Tig1 + (y - 1)*sz.width;
		INT64 * Tu2 = Tig2 + (y - 1)*sz.width;
		INT64 * Tu4 = Tig4 + (y - 1)*sz.width;
		INT64 * Tu8 = Tig8 + (y - 1)*sz.width;
		byte * R1 = Row1 + y*sz.width;
		byte * R2 = Row2 + y*sz.width;
		byte * R4 = Row4 + y*sz.width;
		byte * R8 = Row8 + y*sz.width;
		float *s = scores.ptr<float>(y);
		for (int x = 1; x <= W; x++)
		{
			byte g = G[x - 1];
			R1[x] = (R1[x - 1] << 1) | ((g >> 4) & 1);
			R2[x] = (R2[x - 1] << 1) | ((g >> 5) & 1);
			R4[x] = (R4[x - 1] << 1) | ((g >> 6) & 1);
			R8[x] = (R8[x - 1] << 1) | ((g >> 7) & 1);
			T1[x] = (Tu1[x] << 8) | R1[x];
			T2[x] = (Tu2[x] << 8) | R2[x];
			T4[x] = (Tu4[x] << 8) | R4[x];
			T8[x] = (Tu8[x] << 8) | R8[x];
			s[x] = dot(T1[x], T2[x], T4[x], T8[x]);
		}
	}
	free(Tig1); free(Tig2); free(Tig4); free(Tig8);
	free(Row1); free(Row2); free(Row4); free(Row8);

	scores(Rect(8, 8, W - 7, H - 7)).copyTo(matchCost1f);

}
}  // namespace saliency
}  // namespace cv
