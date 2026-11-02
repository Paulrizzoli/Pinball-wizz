#pragma once

// HD graphics layer.
//
// The original renderer works on a 1x "vScreen" bitmap and a matching z-buffer ("zScreen").
// All game logic, collision and dirty-rectangle bookkeeping keep running at 1x, untouched.
// When an HD pack is loaded, every drawing operation that targets the vScreen, the zScreen or the
// ball save buffers is mirrored at Scale x onto HD "twin" buffers, using HD twins of the source
// bitmaps. The HD vScreen twin is what gets presented.
//
// HD twins of table bitmaps come from the HD pack (HD/<group>.png next to the .dat file), made by
// the hd-pack tool from the player's own data file. Anything without an HD image (fonts, text
// boxes, missing files) falls back to a nearest-neighbour upscale, so the game always renders.

struct gdrv_bitmap8;
struct zmap_header_type;
class DatFile;

class hdgfx
{
public:
	// Current HD scale; 1 means HD is off and nothing is mirrored.
	static int Scale;
	// Scale of the HD pack on disk (may be higher than Scale when a lower detail level is picked).
	static int PackScale;
	// Detail level the "Auto" setting picks for this screen, capped at the pack scale.
	static int AutoScale(int packScale, int tableHeight);

	static bool Active() { return Scale > 1; }

	// Loads HD/pack.ini and HD/<group>.png files for the given table. Returns true on success.
	static bool LoadPack(DatFile* table, const std::string& basePath, int resolution);
	static void Unload();

	// Marks a 1x buffer as a mirroring target (vScreen, zScreen, ball save buffers).
	static void RegisterTarget(gdrv_bitmap8* bmp);
	static void RegisterTarget(zmap_header_type* zMap);
	static bool IsTarget(const gdrv_bitmap8* bmp);
	static bool IsTarget(const zmap_header_type* zMap);

	// HD twin of a 1x buffer, created on demand.
	static gdrv_bitmap8* Twin(gdrv_bitmap8* bmp);
	static zmap_header_type* Twin(zmap_header_type* zMap);

	// Registers a ready-made HD twin for a 1x bitmap (used for bitmaps made at runtime, like LED-tinted lights).
	static void SetTwin(gdrv_bitmap8* bmp, gdrv_bitmap8* twin);
	static void SetTwin(zmap_header_type* zMap, zmap_header_type* twin);

	// Custom tables paint their own HD images: turns HD on at the given scale without a pack.
	static bool BeginCustom(int scale);

	// Called from destructors so a reused address never picks up a stale twin.
	static void Forget(const gdrv_bitmap8* bmp);
	static void Forget(const zmap_header_type* zMap);

	static const std::string& Status() { return StatusText; }

private:
	static std::unordered_map<const gdrv_bitmap8*, gdrv_bitmap8*> BmpTwins;
	static std::unordered_map<const zmap_header_type*, zmap_header_type*> ZMapTwins;
	static std::vector<const void*> Targets;
	static std::string StatusText;
	static bool Clearing;

	static gdrv_bitmap8* UpscaleNearest(const gdrv_bitmap8& bmp);
	static zmap_header_type* UpscaleNearest(const zmap_header_type& zMap);
	static gdrv_bitmap8* LoadPng(const std::string& path, const gdrv_bitmap8& original);
	static void Downscale(const unsigned char* pixels, int packScale, gdrv_bitmap8& twin);
};
