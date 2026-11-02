#include "pch.h"
#include "hdgfx.h"

#include "gdrv.h"
#include "zdrv.h"
#include "GroupData.h"

#include <cctype>
#include <cstdlib>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

int hdgfx::Scale = 1;
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
	Scale = scale;

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

	StatusText = "HD pack " + std::to_string(Scale) + "x: " + std::to_string(loaded) + " images";
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

	if (width != original.Width * Scale || height != original.Height * Scale)
	{
		printf("HD: %s has wrong size %dx%d, expected %dx%d\n", path.c_str(), width, height,
		       original.Width * Scale, original.Height * Scale);
		stbi_image_free(pixels);
		return nullptr;
	}

	auto twin = new gdrv_bitmap8(width, height, false);
	twin->XPosition = original.XPosition * Scale;
	twin->YPosition = original.YPosition * Scale;
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
