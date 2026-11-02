#include "pch.h"
#include "ledfx.h"

#include "gdrv.h"
#include "hdgfx.h"
#include "options.h"
#include "pb.h"
#include "render.h"
#include "TPinballTable.h"
#include "winmain.h"

#include <functional>

std::vector<render_sprite*> ledfx::Lights;
std::unordered_map<const gdrv_bitmap8*, ledfx::BmpInfo> ledfx::Cache;
std::unordered_map<const gdrv_bitmap8*, ledfx::RefBitmap> ledfx::Refs;
std::unordered_map<const gdrv_bitmap8*, gdrv_bitmap8*> ledfx::OffCache;
uint64_t ledfx::LastTicks = 0;

namespace
{
	// Fade times in seconds. LEDs snap on quickly and trail off a little slower.
	constexpr float FadeInTime = 0.07f;
	constexpr float FadeOutTime = 0.16f;

	// Glow texture border around the light bitmap, in 1x pixels.
	constexpr int GlowPad = 14;

	// Brightness of an unlit lamp insert compared to the original art.
	constexpr float OffLevel = 0.6f;

	float Luma(ColorRgba c)
	{
		return 0.299f * c.GetRed() + 0.587f * c.GetGreen() + 0.114f * c.GetBlue();
	}

	ColorRgba PixelAt(const gdrv_bitmap8* bmp, int x, int y)
	{
		if (!bmp || !bmp->BmpBufPtr1 || x < 0 || y < 0 || x >= bmp->Width || y >= bmp->Height)
			return ColorRgba{0};
		return bmp->BmpBufPtr1[bmp->Stride * y + x];
	}

	// "Unlit" colour at a table position, used to tell lamp light apart from the rest of the art.
	using Sampler = std::function<ColorRgba(int x, int y)>;

	// How much a pixel of a light bitmap is "lamp light": 0 for art that matches the table under it,
	// 1 for pixels clearly brighter than the table.
	float LitWeight(ColorRgba pixel, ColorRgba table)
	{
		if (pixel.Color == 0)
			return 0;
		auto diff = Luma(pixel) - Luma(table);
		return std::min(1.0f, std::max(0.0f, (diff - 6.0f) / 45.0f));
	}

	// Pushes a colour towards a saturated, bright LED tone while keeping its hue.
	ColorRgba LedColor(ColorRgba c)
	{
		float rgb[3] = {c.GetRed() / 255.0f, c.GetGreen() / 255.0f, c.GetBlue() / 255.0f};
		auto mx = std::max(rgb[0], std::max(rgb[1], rgb[2]));
		auto mn = std::min(rgb[0], std::min(rgb[1], rgb[2]));
		if (mx <= 0.0f)
			return c;

		auto sat = (mx - mn) / mx;
		float newVal, newSat;
		if (sat < 0.12f)
		{
			// White and grey lamps: just brighter, no tint.
			newVal = std::min(1.0f, mx * 1.12f + 0.04f);
			newSat = sat;
		}
		else
		{
			newVal = std::min(1.0f, mx * 1.18f + 0.06f);
			newSat = std::min(1.0f, sat * 1.3f + 0.1f);
		}

		uint8_t out[3];
		for (int i = 0; i < 3; i++)
		{
			auto k = mx > mn ? (rgb[i] - mn) / (mx - mn) : 1.0f;
			auto v = newVal * (1.0f - newSat * (1.0f - k));
			out[i] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, v * 255.0f + 0.5f)));
		}
		return ColorRgba{out[0], out[1], out[2], 255};
	}

	ColorRgba Lerp(ColorRgba a, ColorRgba b, float t)
	{
		auto mix = [t](uint8_t x, uint8_t y)
		{
			return static_cast<uint8_t>(x + (y - x) * t + 0.5f);
		};
		return ColorRgba{
			mix(a.GetRed(), b.GetRed()), mix(a.GetGreen(), b.GetGreen()), mix(a.GetBlue(), b.GetBlue()), 255
		};
	}

	// LED version of a light bitmap. tableX/tableY is where the bitmap sits on the table bitmap.
	gdrv_bitmap8* MakeLed(const gdrv_bitmap8& src, const Sampler& unlit, int tableX, int tableY)
	{
		auto led = new gdrv_bitmap8(src.Width, src.Height, false);
		led->XPosition = src.XPosition;
		led->YPosition = src.YPosition;
		for (int y = 0; y < src.Height; y++)
		{
			for (int x = 0; x < src.Width; x++)
			{
				auto pixel = src.BmpBufPtr1[src.Stride * y + x];
				auto weight = LitWeight(pixel, unlit(tableX + x, tableY + y));
				led->BmpBufPtr1[led->Stride * y + x] = weight > 0 ? Lerp(pixel, LedColor(pixel), weight) : pixel;
			}
		}
		return led;
	}

	// Unlit LED insert: the table under the lamp, darkened where the lamp's lit bitmap shines.
	gdrv_bitmap8* MakeOff(const gdrv_bitmap8& lit, const gdrv_bitmap8& table, int tableX, int tableY)
	{
		auto off = new gdrv_bitmap8(lit.Width, lit.Height, false);
		off->XPosition = lit.XPosition;
		off->YPosition = lit.YPosition;
		for (int y = 0; y < lit.Height; y++)
		{
			for (int x = 0; x < lit.Width; x++)
			{
				auto under = PixelAt(&table, tableX + x, tableY + y);
				auto weight = LitWeight(lit.BmpBufPtr1[lit.Stride * y + x], under);
				auto dark = ColorRgba{
					static_cast<uint8_t>(under.GetRed() * OffLevel), static_cast<uint8_t>(under.GetGreen() * OffLevel),
					static_cast<uint8_t>(under.GetBlue() * OffLevel), 255
				};
				off->BmpBufPtr1[off->Stride * y + x] = Lerp(under, dark, weight);
			}
		}
		return off;
	}

	void GaussianBlur(std::vector<float>& img, int width, int height, float sigma)
	{
		auto radius = static_cast<int>(std::ceil(sigma * 3));
		std::vector<float> kernel(radius * 2 + 1);
		float sum = 0;
		for (int i = -radius; i <= radius; i++)
			sum += kernel[i + radius] = std::exp(-(i * i) / (2 * sigma * sigma));
		for (auto& k : kernel)
			k /= sum;

		std::vector<float> tmp(img.size());
		for (int y = 0; y < height; y++)
			for (int x = 0; x < width; x++)
				for (int c = 0; c < 3; c++)
				{
					float acc = 0;
					for (int i = -radius; i <= radius; i++)
					{
						auto sx = Clamp(x + i, 0, width - 1);
						acc += img[(y * width + sx) * 3 + c] * kernel[i + radius];
					}
					tmp[(y * width + x) * 3 + c] = acc;
				}
		for (int y = 0; y < height; y++)
			for (int x = 0; x < width; x++)
				for (int c = 0; c < 3; c++)
				{
					float acc = 0;
					for (int i = -radius; i <= radius; i++)
					{
						auto sy = Clamp(y + i, 0, height - 1);
						acc += tmp[(sy * width + x) * 3 + c] * kernel[i + radius];
					}
					img[(y * width + x) * 3 + c] = acc;
				}
	}

	// Soft bloom texture for a light: its lit pixels, blurred at two radii (tight halo + wide glow).
	SDL_Texture* MakeGlow(const gdrv_bitmap8& led, const Sampler& unlit, int tableX, int tableY, float& emission)
	{
		auto width = led.Width + GlowPad * 2, height = led.Height + GlowPad * 2;
		std::vector<float> source(width * height * 3, 0.0f);
		emission = 0;
		for (int y = 0; y < led.Height; y++)
		{
			for (int x = 0; x < led.Width; x++)
			{
				auto pixel = led.BmpBufPtr1[led.Stride * y + x];
				auto weight = LitWeight(pixel, unlit(tableX + x, tableY + y));
				if (weight <= 0)
					continue;
				emission += weight;
				auto dst = &source[((y + GlowPad) * width + x + GlowPad) * 3];
				dst[0] = pixel.GetRed() * weight;
				dst[1] = pixel.GetGreen() * weight;
				dst[2] = pixel.GetBlue() * weight;
			}
		}
		if (emission < 2.0f)
			return nullptr;

		auto halo = source;
		GaussianBlur(halo, width, height, 1.6f);
		GaussianBlur(source, width, height, 5.0f);

		std::vector<ColorRgba> pixels(width * height);
		for (int i = 0; i < width * height; i++)
		{
			uint8_t out[3];
			for (int c = 0; c < 3; c++)
			{
				auto v = halo[i * 3 + c] * 0.6f + source[i * 3 + c] * 2.2f;
				out[c] = static_cast<uint8_t>(std::min(255.0f, v));
			}
			pixels[i] = ColorRgba{out[0], out[1], out[2], 255};
		}

		SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
		auto texture = SDL_CreateTexture(winmain::Renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
		                                 width, height);
		if (!texture)
			return nullptr;
		SDL_UpdateTexture(texture, nullptr, pixels.data(), width * 4);
		SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_ADD);
#if SDL_VERSION_ATLEAST(2, 0, 12)
		SDL_SetTextureScaleMode(texture, SDL_ScaleModeLinear);
#endif
		return texture;
	}
}

void ledfx::Register(render_sprite* sprite, bool dimWhenOff)
{
	if (!sprite || sprite->IsLight)
		return;
	sprite->IsLight = true;
	Lights.push_back(sprite);

	if (dimWhenOff && sprite->Bmp)
	{
		sprite->OffSrcBmp = sprite->Bmp;
		sprite->OffZMap = sprite->ZMap;
		sprite->OffX = sprite->BmpRect.XPosition;
		sprite->OffY = sprite->BmpRect.YPosition;
	}
}

gdrv_bitmap8* ledfx::OffBitmap(const render_sprite& sprite)
{
	auto src = sprite.OffSrcBmp;
	auto table = render::GetTableBitmap();
	if (!src || !sprite.OffZMap || !table || !src->BmpBufPtr1 || src->BitmapType == BitmapTypes::Spliced)
		return nullptr;

	auto it = OffCache.find(src);
	if (it != OffCache.end())
		return it->second;

	auto off = MakeOff(*src, *table, sprite.OffX, sprite.OffY);
	if (hdgfx::Active())
	{
		auto s = hdgfx::Scale;
		hdgfx::SetTwin(off, MakeOff(*hdgfx::Twin(src), *hdgfx::Twin(table), sprite.OffX * s, sprite.OffY * s));
	}
	OffCache[src] = off;
	return off;
}

void ledfx::SetUnlit(gdrv_bitmap8* bmp, RefBitmap unlit)
{
	Refs[bmp] = unlit;
}

void ledfx::Unregister(render_sprite* sprite)
{
	Lights.erase(std::remove(Lights.begin(), Lights.end(), sprite), Lights.end());
}

bool ledfx::Enabled()
{
	return options::Options.LedLighting;
}

bool ledfx::GlowEnabled()
{
	return options::Options.LightGlow && options::Options.LightGlowStrength > 0.0f;
}

float ledfx::Ease(float t)
{
	t = std::min(1.0f, std::max(0.0f, t));
	return t * t * (3.0f - 2.0f * t);
}

ledfx::BmpInfo& ledfx::Info(gdrv_bitmap8* bmp, int xPos, int yPos)
{
	static BmpInfo none{};
	auto table = render::GetTableBitmap();
	if (!bmp || !table || !bmp->BmpBufPtr1 || bmp->BitmapType == BitmapTypes::Spliced)
		return none;

	auto& info = Cache[bmp];
	if (info.Ready)
		return info;
	info.Ready = true;

	// The unlit look is the table under the lamp, or the part's own unlit bitmap when it has one.
	auto refIt = Refs.find(bmp);
	auto ref = refIt != Refs.end() ? &refIt->second : nullptr;
	auto makeSampler = [ref](const gdrv_bitmap8* tableBmp, int scale, bool hd) -> Sampler
	{
		return [=](int x, int y)
		{
			if (ref)
			{
				auto pixel = PixelAt(hd ? hdgfx::Twin(ref->Bmp) : ref->Bmp, x - ref->X * scale, y - ref->Y * scale);
				if (pixel.Color != 0)
					return pixel;
			}
			return PixelAt(tableBmp, x, y);
		};
	};

	auto unlit = makeSampler(table, 1, false);
	info.Led = MakeLed(*bmp, unlit, xPos, yPos);
	if (hdgfx::Active())
	{
		auto s = hdgfx::Scale;
		auto hdUnlit = makeSampler(hdgfx::Twin(table), s, true);
		hdgfx::SetTwin(info.Led, MakeLed(*hdgfx::Twin(bmp), hdUnlit, xPos * s, yPos * s));
	}
	info.Glow = MakeGlow(*info.Led, unlit, xPos, yPos, info.Emission);
	return info;
}

float ledfx::Emission(gdrv_bitmap8* bmp, int xPos, int yPos)
{
	return bmp ? Info(bmp, xPos, yPos).Emission : 0.0f;
}

gdrv_bitmap8* ledfx::LedBitmap(gdrv_bitmap8* bmp, int xPos, int yPos)
{
	auto led = Info(bmp, xPos, yPos).Led;
	return led ? led : bmp;
}

void ledfx::SpriteChanged(render_sprite& sprite, gdrv_bitmap8* bmp, zmap_header_type* zMap, int xPos, int yPos)
{
	if (!Enabled())
	{
		sprite.PrevBmp = nullptr;
		sprite.Fade = 1.0f;
		return;
	}

	auto fading = sprite.Fade < 1.0f;
	if (fading && sprite.PrevBmp == bmp && sprite.PrevX == xPos && sprite.PrevY == yPos)
	{
		// Going back to the bitmap we were fading away from (typical for blinking lights): reverse.
		sprite.PrevBmp = sprite.Bmp;
		sprite.PrevZMap = sprite.ZMap;
		sprite.PrevX = sprite.BmpRect.XPosition;
		sprite.PrevY = sprite.BmpRect.YPosition;
		sprite.Fade = 1.0f - sprite.Fade;
	}
	else
	{
		// Fade from whatever is mostly showing right now.
		if (!fading || sprite.Fade >= 0.5f)
		{
			sprite.PrevBmp = sprite.Bmp;
			sprite.PrevZMap = sprite.ZMap;
			sprite.PrevX = sprite.BmpRect.XPosition;
			sprite.PrevY = sprite.BmpRect.YPosition;
		}
		sprite.Fade = 0.0f;
	}

	if (!sprite.PrevBmp && !bmp)
	{
		sprite.Fade = 1.0f;
		return;
	}

	auto brighter = Emission(bmp, xPos, yPos) >= Emission(sprite.PrevBmp, sprite.PrevX, sprite.PrevY);
	sprite.FadeTime = brighter ? FadeInTime : FadeOutTime;
}

void ledfx::Update()
{
	auto now = SDL_GetTicks64();
	auto dt = LastTicks ? std::min(0.1f, (now - LastTicks) / 1000.0f) : 0.0f;
	LastTicks = now;

	for (auto sprite : Lights)
	{
		if (sprite->Fade >= 1.0f)
			continue;

		sprite->DirtyFlag = true;
		sprite->Fade += sprite->FadeTime > 0 ? dt / sprite->FadeTime : 1.0f;
		if (sprite->Fade >= 1.0f || !Enabled())
		{
			sprite->Fade = 1.0f;
			sprite->PrevBmp = nullptr;
		}
	}
}

void ledfx::DrawGlowLayer(gdrv_bitmap8* bmp, int xPos, int yPos, float weight, float originX, float originY,
                          float scaleX, float scaleY)
{
	if (weight <= 0.004f)
		return;
	auto& info = Info(bmp, xPos, yPos);
	if (!info.Glow)
		return;

	auto x = originX + (xPos - GlowPad) * scaleX;
	auto y = originY + (yPos - GlowPad) * scaleY;
	auto w = (bmp->Width + GlowPad * 2) * scaleX;
	auto h = (bmp->Height + GlowPad * 2) * scaleY;

	// Strength above 1 is drawn as a second pass, since alpha tops out at 255.
	while (weight > 0.004f)
	{
		auto pass = std::min(weight, 1.0f);
		weight -= pass;
		SDL_SetTextureAlphaMod(info.Glow, static_cast<uint8_t>(pass * 255.0f + 0.5f));
#if SDL_VERSION_ATLEAST(2, 0, 10)
		SDL_FRect dst{x, y, w, h};
		SDL_RenderCopyF(winmain::Renderer, info.Glow, nullptr, &dst);
#else
		SDL_Rect dst{
			static_cast<int>(x), static_cast<int>(y), static_cast<int>(w + 0.5f), static_cast<int>(h + 0.5f)
		};
		SDL_RenderCopy(winmain::Renderer, info.Glow, nullptr, &dst);
#endif
	}
}

void ledfx::DrawGlow()
{
	if (!GlowEnabled() || Lights.empty() || !render::vscreen)
		return;

	float originX, originY, scaleX, scaleY;
	SDL_Rect clip;
	render::GetBoardTransform(originX, originY, scaleX, scaleY, clip);
	SDL_RenderSetClipRect(winmain::Renderer, &clip);

	float strength = options::Options.LightGlowStrength;
	for (auto sprite : Lights)
	{
		if (sprite->DeleteFlag)
			continue;

		auto fade = Enabled() ? Ease(sprite->Fade) : 1.0f;
		if (sprite->Bmp)
			DrawGlowLayer(sprite->Bmp, sprite->BmpRect.XPosition, sprite->BmpRect.YPosition, fade * strength,
			              originX, originY, scaleX, scaleY);
		if (sprite->PrevBmp && sprite->Fade < 1.0f)
			DrawGlowLayer(sprite->PrevBmp, sprite->PrevX, sprite->PrevY, (1.0f - fade) * strength,
			              originX, originY, scaleX, scaleY);
	}

	SDL_RenderSetClipRect(winmain::Renderer, nullptr);
}

void ledfx::SettingsChanged()
{
	for (auto sprite : Lights)
	{
		sprite->Fade = 1.0f;
		sprite->PrevBmp = nullptr;
		sprite->DirtyFlag = true;
	}
	if (pb::MainTable)
		render::update();
}

void ledfx::Uninit()
{
	for (auto& pair : Cache)
	{
		delete pair.second.Led;
		if (pair.second.Glow)
			SDL_DestroyTexture(pair.second.Glow);
	}
	Cache.clear();
	for (auto& pair : OffCache)
		delete pair.second;
	OffCache.clear();
	Refs.clear();
	Lights.clear();
	LastTicks = 0;
}
