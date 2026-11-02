#include "pch.h"
#include "zdrv.h"
#include "winmain.h"
#include "hdgfx.h"


zmap_header_type::zmap_header_type(int width, int height, int stride)
{
	Resolution = 0;
	Width = width;
	Height = height;
	Stride = stride >= 0 ? stride : pad(width);
	Texture = nullptr;
	ZPtr1 = new unsigned short[Stride * Height];
}

zmap_header_type::~zmap_header_type()
{
	hdgfx::Forget(this);
	delete[] ZPtr1;
	if (Texture)
		SDL_DestroyTexture(Texture);
}

int zmap_header_type::pad(int width)
{
	int result = width;
	if (width & 3)
		result = width - (width & 3) + 4;
	return result;
}


void zdrv::fill(zmap_header_type* zmap, int width, int height, int xOff, int yOff, uint16_t fillWord)
{
	if (hdgfx::IsTarget(zmap))
	{
		auto s = hdgfx::Scale;
		fill(hdgfx::Twin(zmap), width * s, height * s, xOff * s, yOff * s, fillWord);
	}

	auto dstPtr = &zmap->ZPtr1[zmap->Stride * yOff + xOff];
	for (int y = height; y > 0; --y)
	{
		for (int x = width; x > 0; --x)
		{
			*dstPtr++ = fillWord;
		}
		dstPtr += zmap->Stride - width;
	}
}


void zdrv::paint(int width, int height, gdrv_bitmap8* dstBmp, int dstBmpXOff, int dstBmpYOff, zmap_header_type* dstZMap,
                 int dstZMapXOff, int dstZMapYOff, gdrv_bitmap8* srcBmp, int srcBmpXOff, int srcBmpYOff,
                 zmap_header_type* srcZMap, int srcZMapXOff, int srcZMapYOff)
{
	if (hdgfx::IsTarget(dstBmp))
	{
		auto s = hdgfx::Scale;
		paint(width * s, height * s, hdgfx::Twin(dstBmp), dstBmpXOff * s, dstBmpYOff * s,
		      hdgfx::Twin(dstZMap), dstZMapXOff * s, dstZMapYOff * s, hdgfx::Twin(srcBmp), srcBmpXOff * s,
		      srcBmpYOff * s, hdgfx::Twin(srcZMap), srcZMapXOff * s, srcZMapYOff * s);
	}

	assertm(srcBmp->BitmapType != BitmapTypes::Spliced, "Wrong bmp type");

	auto srcPtr = &srcBmp->BmpBufPtr1[srcBmp->Stride * srcBmpYOff + srcBmpXOff];
	auto dstPtr = &dstBmp->BmpBufPtr1[dstBmp->Stride * dstBmpYOff + dstBmpXOff];
	auto srcPtrZ = &srcZMap->ZPtr1[srcZMap->Stride * srcZMapYOff + srcZMapXOff];
	auto dstPtrZ = &dstZMap->ZPtr1[dstZMap->Stride * dstZMapYOff + dstZMapXOff];

	for (int y = height; y > 0; y--)
	{
		for (int x = width; x > 0; --x)
		{
			if (*dstPtrZ >= *srcPtrZ)
			{
				*dstPtr = *srcPtr;
				*dstPtrZ = *srcPtrZ;
			}
			++srcPtr;
			++dstPtr;
			++srcPtrZ;
			++dstPtrZ;
		}

		srcPtr += srcBmp->Stride - width;
		dstPtr += dstBmp->Stride - width;
		srcPtrZ += srcZMap->Stride - width;
		dstPtrZ += dstZMap->Stride - width;
	}
}

void zdrv::paint_alpha(int width, int height, gdrv_bitmap8* dstBmp, int dstBmpXOff, int dstBmpYOff,
                       zmap_header_type* dstZMap, int dstZMapXOff, int dstZMapYOff, gdrv_bitmap8* srcBmp,
                       int srcBmpXOff, int srcBmpYOff, zmap_header_type* srcZMap, int srcZMapXOff, int srcZMapYOff,
                       uint8_t alpha)
{
	if (alpha == 255)
	{
		paint(width, height, dstBmp, dstBmpXOff, dstBmpYOff, dstZMap, dstZMapXOff, dstZMapYOff, srcBmp, srcBmpXOff,
		      srcBmpYOff, srcZMap, srcZMapXOff, srcZMapYOff);
		return;
	}

	if (hdgfx::IsTarget(dstBmp))
	{
		auto s = hdgfx::Scale;
		paint_alpha(width * s, height * s, hdgfx::Twin(dstBmp), dstBmpXOff * s, dstBmpYOff * s,
		            hdgfx::Twin(dstZMap), dstZMapXOff * s, dstZMapYOff * s, hdgfx::Twin(srcBmp), srcBmpXOff * s,
		            srcBmpYOff * s, hdgfx::Twin(srcZMap), srcZMapXOff * s, srcZMapYOff * s, alpha);
	}

	assertm(srcBmp->BitmapType != BitmapTypes::Spliced, "Wrong bmp type");

	const unsigned a = alpha, ia = 255 - alpha;
	for (int y = 0; y < height; y++)
	{
		auto srcPtr = &srcBmp->BmpBufPtr1[srcBmp->Stride * (srcBmpYOff + y) + srcBmpXOff];
		auto dstPtr = &dstBmp->BmpBufPtr1[dstBmp->Stride * (dstBmpYOff + y) + dstBmpXOff];
		auto srcPtrZ = &srcZMap->ZPtr1[srcZMap->Stride * (srcZMapYOff + y) + srcZMapXOff];
		auto dstPtrZ = &dstZMap->ZPtr1[dstZMap->Stride * (dstZMapYOff + y) + dstZMapXOff];
		for (int x = 0; x < width; x++)
		{
			if (dstPtrZ[x] >= srcPtrZ[x])
			{
				auto src = srcPtr[x], dst = dstPtr[x];
				dstPtr[x] = ColorRgba{
					static_cast<uint8_t>((src.GetRed() * a + dst.GetRed() * ia + 127) / 255),
					static_cast<uint8_t>((src.GetGreen() * a + dst.GetGreen() * ia + 127) / 255),
					static_cast<uint8_t>((src.GetBlue() * a + dst.GetBlue() * ia + 127) / 255),
					255
				};
				dstPtrZ[x] = srcPtrZ[x];
			}
		}
	}
}

void zdrv::paint_flat(int width, int height, gdrv_bitmap8* dstBmp, int dstBmpXOff, int dstBmpYOff,
                      zmap_header_type* zMap, int dstZMapXOff, int dstZMapYOff, gdrv_bitmap8* srcBmp, int srcBmpXOff,
                      int srcBmpYOff, uint16_t depth)
{
	if (hdgfx::IsTarget(dstBmp))
	{
		auto s = hdgfx::Scale;
		paint_flat(width * s, height * s, hdgfx::Twin(dstBmp), dstBmpXOff * s, dstBmpYOff * s,
		           hdgfx::Twin(zMap), dstZMapXOff * s, dstZMapYOff * s, hdgfx::Twin(srcBmp), srcBmpXOff * s,
		           srcBmpYOff * s, depth);
	}

	assertm(srcBmp->BitmapType != BitmapTypes::Spliced, "Wrong bmp type");

	auto dstPtr = &dstBmp->BmpBufPtr1[dstBmp->Stride * dstBmpYOff + dstBmpXOff];
	auto srcPtr = &srcBmp->BmpBufPtr1[srcBmp->Stride * srcBmpYOff + srcBmpXOff];
	auto zPtr = &zMap->ZPtr1[zMap->Stride * dstZMapYOff + dstZMapXOff];

	for (int y = height; y > 0; y--)
	{
		for (int x = width; x > 0; --x)
		{
			if ((*srcPtr).Color && *zPtr > depth)
			{
				*dstPtr = *srcPtr;
			}
			++srcPtr;
			++dstPtr;
			++zPtr;
		}

		srcPtr += srcBmp->Stride - width;
		dstPtr += dstBmp->Stride - width;
		zPtr += zMap->Stride - width;
	}
}

void zdrv::CreatePreview(zmap_header_type& zMap)
{
	if (zMap.Texture)
		return;

	auto tmpBuff = new ColorRgba[zMap.Width * zMap.Height];

	auto dst = tmpBuff;
	auto src = zMap.ZPtr1;
	for (auto y = 0; y < zMap.Height; y++)
	{
		for (auto x = 0; x < zMap.Width; x++)
		{
			auto depth = static_cast<uint8_t>((0xffff - *src++) / 0xff);
			*dst++ = ColorRgba{ depth, depth, depth, 0xff };
		}
		src += zMap.Stride - zMap.Width;
	}

	auto texture = SDL_CreateTexture
	(
		winmain::Renderer,
		SDL_PIXELFORMAT_ARGB8888,
		SDL_TEXTUREACCESS_STATIC,
		zMap.Width, zMap.Height
	);
	SDL_UpdateTexture(texture, nullptr, tmpBuff, zMap.Width * 4);
	zMap.Texture = texture;
	delete[] tmpBuff;
}

void zdrv::FlipZMapHorizontally(const zmap_header_type& zMap)
{
	// Flip in-place, iterate over Height/2 lines
	auto dst = zMap.ZPtr1;
	auto src = zMap.ZPtr1 + zMap.Stride * (zMap.Height - 1);
	for (auto y = zMap.Height - 1; y >= zMap.Height / 2; y--)
	{
		for (auto x = 0; x < zMap.Width; x++)
		{
			std::swap(*dst++, *src++);
		}
		dst += zMap.Stride - zMap.Width;
		src -= zMap.Stride + zMap.Width;
	}
}
