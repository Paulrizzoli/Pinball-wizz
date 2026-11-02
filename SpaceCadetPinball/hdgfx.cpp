#include "pch.h"
#include "hdgfx.h"

#include "gdrv.h"
#include "zdrv.h"
#include "GroupData.h"
#include "fullscrn.h"
#include "options.h"
#include "winmain.h"

#include <cctype>
#include <cstdlib>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

int hdgfx::Scale = 1;
int hdgfx::PackScale = 0;
std::unordered_map<const gdrv_bitmap8*, gdrv_bitmap8*> hdgfx::BmpTwins;
std::unordered_map<const zmap_header_type*, zmap_header_type*> hdgfx::ZMapTwins;
std::vector<const void*> hdgfx::Targets;
std::string hdgfx::StatusText = "HD pack not loaded";
bool hdgfx::Clearing = false;

namespace
{
	bool ReadWholeFile(const std::string& path, std::vector<unsigned char>& data)
	{
		auto rw = SDL_RWFromFile(path.c_str(), "rb");
		if (!rw)
			return false;
		auto size = SDL_RWsize(rw);
		if (size <= 0)
		{
			SDL_RWclose(rw);
			return false;
		}
		data.resize(static_cast<size_t>(size));
		auto read = SDL_RWread(rw, data.data(), 1, data.size());
		SDL_RWclose(rw);
		return read == data.size();
	}

	int ParseScale(const std::string& ini)
	{
		// Minimal "key=value" parser, only "scale" is needed.
		size_t pos = 0;
		while (pos < ini.size())
		{
			auto end = ini.find('\n', pos);
			if (end == std::string::npos)
				end = ini.size();
			auto line = ini.substr(pos, end - pos);
			pos = end + 1;

			auto eq = line.find('=');
			if (eq == std::string::npos)
				continue;
			auto key = line.substr(0, eq);
			key.erase(std::remove_if(key.begin(), key.end(), ::isspace), key.end());
			if (key == "scale")
				return std::atoi(line.c_str() + eq + 1);
		}
		return 0;
	}
}

bool hdgfx::LoadPack(DatFile* table, const std::string& basePath, int resolution)
{
	PackScale = 0;
	Unload();

	if (resolution != 0)
	{
		StatusText = "HD pack disabled: only the 640x480 table resolution is supported";
		return false;
	}

	std::vector<unsigned char> iniData;
	auto hdDir = basePath + "HD/";
	if (!ReadWholeFile(hdDir + "pack.ini", iniData))
	{
		SDL_ClearError();
		StatusText = "No HD pack found (HD/pack.ini missing)";
		return false;
	}

	auto scale = ParseScale(std::string(iniData.begin(), iniData.end()));
	if (scale < 2 || scale > 8)
	{
		StatusText = "HD pack has an invalid scale";
		return false;
	}
	PackScale = scale;

	// Detail level: the pack is drawn at a lower scale when asked, which is much lighter on slower PCs.
	auto detail = options::Options.HdDetail.V;
	auto tableHeight = fullscrn::resolution_array[resolution].TableHeight;
	auto wanted = detail <= 0 ? AutoScale(scale, tableHeight) : std::min(detail, scale);
	if (wanted < 2)
	{
		StatusText = "HD pack found, detail set to original art";
		return false;
	}
	Scale = wanted;

	int loaded = 0;
	for (auto group : table->Groups)
	{
		if (!group)
			continue;
		auto bmp = group->GetBitmap(resolution);
		if (!bmp || bmp->BitmapType == BitmapTypes::None)
			continue;

		char name[32];
		snprintf(name, sizeof name, "%03d.png", group->GroupId);
		auto hdBmp = LoadPng(hdDir + name, *bmp);
		// Bitmaps without a file (such as the built-in font) are upscaled in-game when first drawn.
		if (hdBmp)
		{
			BmpTwins[bmp] = hdBmp;
			loaded++;
		}
	}
	SDL_ClearError();

	StatusText = "HD pack " + std::to_string(PackScale) + "x, drawn at " + std::to_string(Scale) + "x: " +
		std::to_string(loaded) + " images";
	printf("%s\n", StatusText.c_str());
	return true;
}

void hdgfx::Unload()
{
	Clearing = true;
	for (auto& pair : BmpTwins)
		delete pair.second;
	for (auto& pair : ZMapTwins)
		delete pair.second;
	Clearing = false;
	BmpTwins.clear();
	ZMapTwins.clear();
	Targets.clear();
	Scale = 1;
}

void hdgfx::RegisterTarget(gdrv_bitmap8* bmp)
{
	if (Active() && bmp)
		Targets.push_back(bmp);
}

void hdgfx::RegisterTarget(zmap_header_type* zMap)
{
	if (Active() && zMap)
		Targets.push_back(zMap);
}

bool hdgfx::IsTarget(const gdrv_bitmap8* bmp)
{
	return Active() && std::find(Targets.begin(), Targets.end(), bmp) != Targets.end();
}

bool hdgfx::IsTarget(const zmap_header_type* zMap)
{
	return Active() && std::find(Targets.begin(), Targets.end(), zMap) != Targets.end();
}

gdrv_bitmap8* hdgfx::Twin(gdrv_bitmap8* bmp)
{
	auto it = BmpTwins.find(bmp);
	if (it != BmpTwins.end())
		return it->second;

	auto twin = UpscaleNearest(*bmp);
	BmpTwins[bmp] = twin;
	return twin;
}

zmap_header_type* hdgfx::Twin(zmap_header_type* zMap)
{
	auto it = ZMapTwins.find(zMap);
	if (it != ZMapTwins.end())
		return it->second;

	auto twin = UpscaleNearest(*zMap);
	ZMapTwins[zMap] = twin;
	return twin;
}

void hdgfx::SetTwin(gdrv_bitmap8* bmp, gdrv_bitmap8* twin)
{
	auto it = BmpTwins.find(bmp);
	if (it != BmpTwins.end())
		delete it->second;
	BmpTwins[bmp] = twin;
}

void hdgfx::SetTwin(zmap_header_type* zMap, zmap_header_type* twin)
{
	auto it = ZMapTwins.find(zMap);
	if (it != ZMapTwins.end())
		delete it->second;
	ZMapTwins[zMap] = twin;
}

bool hdgfx::BeginCustom(int scale)
{
	PackScale = 0;
	Unload();
	if (scale < 2)
	{
		StatusText = "Custom table drawn at original detail";
		return false;
	}
	PackScale = scale;
	Scale = scale;
	StatusText = "Custom table drawn at " + std::to_string(Scale) + "x";
	printf("%s\n", StatusText.c_str());
	return true;
}

void hdgfx::Forget(const gdrv_bitmap8* bmp)
{
	if (Clearing)
		return;
	auto it = BmpTwins.find(bmp);
	if (it != BmpTwins.end())
	{
		auto twin = it->second;
		BmpTwins.erase(it);
		delete twin;
	}
	Targets.erase(std::remove(Targets.begin(), Targets.end(), bmp), Targets.end());
}

void hdgfx::Forget(const zmap_header_type* zMap)
{
	if (Clearing)
		return;
	auto it = ZMapTwins.find(zMap);
	if (it != ZMapTwins.end())
	{
		auto twin = it->second;
		ZMapTwins.erase(it);
		delete twin;
	}
	Targets.erase(std::remove(Targets.begin(), Targets.end(), zMap), Targets.end());
}

gdrv_bitmap8* hdgfx::UpscaleNearest(const gdrv_bitmap8& bmp)
{
	auto twin = new gdrv_bitmap8(bmp.Width * Scale, bmp.Height * Scale, false);
	twin->XPosition = bmp.XPosition * Scale;
	twin->YPosition = bmp.YPosition * Scale;
	if (!bmp.BmpBufPtr1)
	{
		std::memset(twin->BmpBufPtr1, 0, sizeof(ColorRgba) * twin->Stride * twin->Height);
		return twin;
	}

	for (int y = 0; y < twin->Height; y++)
	{
		auto src = &bmp.BmpBufPtr1[bmp.Stride * (y / Scale)];
		auto dst = &twin->BmpBufPtr1[twin->Stride * y];
		for (int x = 0; x < twin->Width; x++)
			dst[x] = src[x / Scale];
	}
	return twin;
}

zmap_header_type* hdgfx::UpscaleNearest(const zmap_header_type& zMap)
{
	auto twin = new zmap_header_type(zMap.Width * Scale, zMap.Height * Scale, zMap.Width * Scale);
	for (int y = 0; y < twin->Height; y++)
	{
		auto src = &zMap.ZPtr1[zMap.Stride * (y / Scale)];
		auto dst = &twin->ZPtr1[twin->Stride * y];
		for (int x = 0; x < twin->Width; x++)
			dst[x] = src[x / Scale];
	}
	return twin;
}

gdrv_bitmap8* hdgfx::LoadPng(const std::string& path, const gdrv_bitmap8& original)
{
	std::vector<unsigned char> fileData;
	if (!ReadWholeFile(path, fileData))
		return nullptr;

	int width, height, channels;
	auto pixels = stbi_load_from_memory(fileData.data(), static_cast<int>(fileData.size()),
	                                    &width, &height, &channels, 4);
	if (!pixels)
		return nullptr;

	if (width != original.Width * PackScale || height != original.Height * PackScale)
	{
		printf("HD: %s has wrong size %dx%d, expected %dx%d\n", path.c_str(), width, height,
		       original.Width * PackScale, original.Height * PackScale);
		stbi_image_free(pixels);
		return nullptr;
	}

	auto twin = new gdrv_bitmap8(original.Width * Scale, original.Height * Scale, false);
	twin->XPosition = original.XPosition * Scale;
	twin->YPosition = original.YPosition * Scale;
	if (Scale != PackScale)
	{
		Downscale(pixels, PackScale, *twin);
		stbi_image_free(pixels);
		return twin;
	}
	auto src = pixels;
	auto dst = twin->BmpBufPtr1;
	for (int i = 0; i < width * height; i++, src += 4)
	{
		// Fully transparent pixels must be exactly 0, the same as palette index 0 in the original.
		if (src[3] < 128)
			*dst++ = ColorRgba{0};
		else
			*dst++ = ColorRgba{src[0], src[1], src[2], 0xff};
	}

	stbi_image_free(pixels);
	return twin;
}

int hdgfx::AutoScale(int packScale, int tableHeight)
{
	// Smallest scale whose table height still covers most of the screen height: sharp, without drawing
	// far more pixels than the screen can show.
	SDL_DisplayMode mode{};
	auto display = winmain::MainWindow ? SDL_GetWindowDisplayIndex(winmain::MainWindow) : 0;
	if (SDL_GetDesktopDisplayMode(display < 0 ? 0 : display, &mode) != 0 || mode.h <= 0 || tableHeight <= 0)
		return std::min(packScale, 2);
	int scale = 2;
	while (scale < packScale && tableHeight * scale < mode.h * 85 / 100)
		scale++;
	return scale;
}

void hdgfx::Downscale(const unsigned char* pixels, int packScale, gdrv_bitmap8& twin)
{
	// Area average from the pack scale down to Scale. A pixel is opaque when most of its area is.
	auto srcW = twin.Width / Scale * packScale;
	for (int y = 0; y < twin.Height; y++)
	{
		auto y0 = y * packScale / Scale, y1 = std::max(y0 + 1, (y + 1) * packScale / Scale);
		for (int x = 0; x < twin.Width; x++)
		{
			auto x0 = x * packScale / Scale, x1 = std::max(x0 + 1, (x + 1) * packScale / Scale);
			unsigned r = 0, g = 0, b = 0, opaque = 0, total = 0;
			for (int sy = y0; sy < y1; sy++)
			{
				auto src = pixels + (static_cast<size_t>(sy) * srcW + x0) * 4;
				for (int sx = x0; sx < x1; sx++, src += 4)
				{
					total++;
					if (src[3] < 128)
						continue;
					opaque++;
					r += src[0];
					g += src[1];
					b += src[2];
				}
			}
			auto& dst = twin.BmpBufPtr1[twin.Stride * y + x];
			if (opaque * 2 < total)
				dst = ColorRgba{0};
			else
				dst = ColorRgba{
					static_cast<uint8_t>(r / opaque), static_cast<uint8_t>(g / opaque),
					static_cast<uint8_t>(b / opaque), 0xff
				};
		}
	}
}
