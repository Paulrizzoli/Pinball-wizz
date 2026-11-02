#include "pch.h"
#include "customtable.h"

#include "GroupData.h"
#include "gdrv.h"
#include "zdrv.h"
#include "hdgfx.h"
#include "minijson.h"
#include "tableart.h"
#include "TPinballComponent.h"
#include "options.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>

#include "stb_image.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#endif

bool customtable::Active = false;
std::string customtable::Name;
std::string customtable::Path;
int customtable::BallsPerGame = 3;
std::map<int, int> customtable::Scores;
std::map<int, int> customtable::Bonus;
CustomRules customtable::Rules;
int customtable::BuildScale = 1;
std::vector<std::string> customtable::SearchFolders;

using minijson::Value;

namespace
{
	float Clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

	// ---------------------------------------------------------------------------------------------
	// Files

	bool ReadFileBytes(const std::string& path, std::string& data)
	{
		auto file = fopenu(path.c_str(), "rb");
		if (!file)
			return false;
		fseek(file, 0, SEEK_END);
		auto size = ftell(file);
		fseek(file, 0, SEEK_SET);
		if (size < 0)
		{
			fclose(file);
			return false;
		}
		data.resize(static_cast<size_t>(size));
		auto read = size ? fread(&data[0], 1, data.size(), file) : 0;
		fclose(file);
		return read == data.size();
	}

	uint16_t Read16(const std::string& d, size_t pos)
	{
		return static_cast<uint16_t>(static_cast<uint8_t>(d[pos]) | static_cast<uint8_t>(d[pos + 1]) << 8);
	}

	uint32_t Read32(const std::string& d, size_t pos)
	{
		return static_cast<uint32_t>(Read16(d, pos)) | static_cast<uint32_t>(Read16(d, pos + 2)) << 16;
	}

	// Reads every file of a zip archive (stored or deflated).
	bool ReadZip(const std::string& zip, std::map<std::string, std::string>& files, std::string& error)
	{
		if (zip.size() < 22)
		{
			error = "The table file is too short to be a zip archive";
			return false;
		}
		// End of central directory record, searched from the end (it may be followed by a comment).
		size_t eocd = std::string::npos;
		auto start = zip.size() >= 22 + 65535 ? zip.size() - 22 - 65535 : 0;
		for (size_t pos = zip.size() - 22 + 1; pos-- > start;)
		{
			if (Read32(zip, pos) == 0x06054b50)
			{
				eocd = pos;
				break;
			}
		}
		if (eocd == std::string::npos)
		{
			error = "The table file is not a valid zip archive";
			return false;
		}
		auto count = Read16(zip, eocd + 10);
		size_t pos = Read32(zip, eocd + 16);
		for (int i = 0; i < count; i++)
		{
			if (pos + 46 > zip.size() || Read32(zip, pos) != 0x02014b50)
			{
				error = "The table file's zip directory is damaged";
				return false;
			}
			auto method = Read16(zip, pos + 10);
			auto compSize = Read32(zip, pos + 20);
			auto size = Read32(zip, pos + 24);
			auto nameLen = Read16(zip, pos + 28);
			auto extraLen = Read16(zip, pos + 30);
			auto commentLen = Read16(zip, pos + 32);
			auto localPos = Read32(zip, pos + 42);
			auto name = zip.substr(pos + 46, nameLen);
			pos += 46 + nameLen + extraLen + commentLen;

			if (name.empty() || name.back() == '/')
				continue;
			if (localPos + 30 > zip.size() || Read32(zip, localPos) != 0x04034b50)
			{
				error = "The table file's zip entry for " + name + " is damaged";
				return false;
			}
			auto dataPos = localPos + 30 + Read16(zip, localPos + 26) + Read16(zip, localPos + 28);
			if (dataPos + compSize > zip.size())
			{
				error = "The table file is cut short";
				return false;
			}
			if (method == 0)
			{
				files[name] = zip.substr(dataPos, compSize);
			}
			else if (method == 8)
			{
				std::string out(size, '\0');
				if (size > 0)
				{
					auto got = stbi_zlib_decode_noheader_buffer(&out[0], static_cast<int>(size),
					                                            zip.data() + dataPos, static_cast<int>(compSize));
					if (got != static_cast<int>(size))
					{
						error = "Could not unpack " + name + " from the table file";
						return false;
					}
				}
				files[name] = out;
			}
			else
			{
				error = "The table file uses an unsupported zip compression for " + name;
				return false;
			}
		}
		return true;
	}

	bool ReadTableDocument(const std::string& path, Value& doc, std::map<std::string, std::string>& files,
	                       std::string& error)
	{
		std::string data;
		if (!ReadFileBytes(path, data))
		{
			error = "Could not open the table file";
			return false;
		}

		std::string text;
		if (data.size() >= 4 && data[0] == 'P' && data[1] == 'K')
		{
			if (!ReadZip(data, files, error))
				return false;
			auto it = files.find("table.json");
			if (it == files.end())
			{
				error = "The table file has no table.json inside";
				return false;
			}
			text = it->second;
		}
		else
		{
			text = data;
		}

		if (!minijson::Parse(text, doc, error))
		{
			error = "The table description could not be read: " + error;
			return false;
		}
		if (!doc.IsObject() || doc["format"].Str() != "pinball-wizz-table")
		{
			error = "This is not a Pinball Wizz table file";
			return false;
		}
		if (doc["version"].Int(1) > 1)
		{
			error = "This table was made with a newer version of Pinball Wizz. Update the game to play it.";
			return false;
		}
		return true;
	}

	// ---------------------------------------------------------------------------------------------
	// Records

	struct PendingArt
	{
		gdrv_bitmap8* Bmp;
		zmap_header_type* ZMap;
		ArtImage One;
		ArtImage Hd;
	};

	std::vector<PendingArt> Pending;

	struct GroupBuild
	{
		std::vector<EntryData*> Entries;

		void Add(FieldTypes type, char* buffer, int size)
		{
			auto entry = new EntryData(type, buffer);
			entry->FieldSize = size;
			Entries.push_back(entry);
		}

		void ShortValue(int16_t v)
		{
			auto buf = new char[2];
			std::memcpy(buf, &v, 2);
			Add(FieldTypes::ShortValue, buf, 2);
		}

		void Name(const std::string& name)
		{
			auto buf = new char[name.size() + 1];
			std::memcpy(buf, name.c_str(), name.size() + 1);
			Add(FieldTypes::GroupName, buf, static_cast<int>(name.size() + 1));
		}

		void Shorts(const std::vector<int>& values)
		{
			auto buf = new char[values.size() * 2];
			for (size_t i = 0; i < values.size(); i++)
			{
				auto v = static_cast<int16_t>(values[i]);
				std::memcpy(buf + i * 2, &v, 2);
			}
			Add(FieldTypes::ShortArray, buf, static_cast<int>(values.size() * 2));
		}

		void Floats(const std::vector<float>& values)
		{
			auto buf = new char[values.size() * 4];
			std::memcpy(buf, values.data(), values.size() * 4);
			Add(FieldTypes::FloatArray, buf, static_cast<int>(values.size() * 4));
		}

		void Sprite(gdrv_bitmap8* bmp, zmap_header_type* zMap)
		{
			Add(FieldTypes::Bitmap8bit, reinterpret_cast<char*>(bmp), -1);
			if (zMap)
				Add(FieldTypes::Bitmap16bit, reinterpret_cast<char*>(zMap), -1);
		}
	};

	int Commit(DatFile* dat, GroupBuild& build)
	{
		auto index = static_cast<int>(dat->Groups.size());
		auto group = new GroupData(index);
		std::stable_sort(build.Entries.begin(), build.Entries.end(), [](const EntryData* a, const EntryData* b)
		{
			return a->EntryType < b->EntryType;
		});
		for (auto entry : build.Entries)
			group->AddEntry(entry);
		build.Entries.clear();
		dat->Groups.push_back(group);
		return index;
	}

	// Turns painted images into a game bitmap (and zMap when the image has depth) at screen position x, y.
	void MakeSprite(const ArtImage& one, const ArtImage& hd, int x, int y, bool withDepth,
	                gdrv_bitmap8*& bmp, zmap_header_type*& zMap)
	{
		bmp = new gdrv_bitmap8(one.Width, one.Height, true);
		bmp->XPosition = x;
		bmp->YPosition = y;
		std::memset(bmp->IndexedBmpPtr, 0, static_cast<size_t>(bmp->IndexedStride) * bmp->Height);
		zMap = nullptr;
		if (withDepth)
		{
			zMap = new zmap_header_type(one.Width, one.Height, -1);
			for (int row = 0; row < one.Height; row++)
				std::memcpy(&zMap->ZPtr1[zMap->Stride * row], &one.Depth[static_cast<size_t>(row) * one.Width],
				            one.Width * 2);
			// Group data flips zMaps when they are added (the .dat stores them bottom-up).
			zdrv::FlipZMapHorizontally(*zMap);
		}
		Pending.push_back({bmp, zMap, one, hd});
	}

	// ---------------------------------------------------------------------------------------------
	// Geometry

	struct P2
	{
		float X, Y;
	};

	P2 ReadPoint(const Value& v)
	{
		return {v[static_cast<size_t>(0)].Float(), v[static_cast<size_t>(1)].Float()};
	}

	std::vector<P2> ReadPoints(const Value& v)
	{
		std::vector<P2> pts;
		for (auto& item : v.Items())
			pts.push_back(ReadPoint(item));
		return pts;
	}

	float SignedArea(const std::vector<P2>& poly)
	{
		float area = 0;
		for (size_t i = 0; i < poly.size(); i++)
		{
			auto& p = poly[i];
			auto& q = poly[(i + 1) % poly.size()];
			area += p.X * q.Y - q.X * p.Y;
		}
		return area * 0.5f;
	}

	// Collision outlines must wind so that every edge faces outwards (positive area in table coordinates).
	std::vector<P2> Outward(std::vector<P2> poly)
	{
		if (SignedArea(poly) < 0)
			std::reverse(poly.begin(), poly.end());
		return poly;
	}

	// Outline of a wall drawn as a line of points with a thickness.
	std::vector<P2> ThickLine(const std::vector<P2>& pts, float halfWidth)
	{
		std::vector<P2> left, right;
		auto n = pts.size();
		for (size_t i = 0; i < n; i++)
		{
			P2 dirIn{0, 0}, dirOut{0, 0};
			if (i > 0)
			{
				dirIn = {pts[i].X - pts[i - 1].X, pts[i].Y - pts[i - 1].Y};
				auto len = std::sqrt(dirIn.X * dirIn.X + dirIn.Y * dirIn.Y);
				if (len > 0)
					dirIn = {dirIn.X / len, dirIn.Y / len};
			}
			if (i + 1 < n)
			{
				dirOut = {pts[i + 1].X - pts[i].X, pts[i + 1].Y - pts[i].Y};
				auto len = std::sqrt(dirOut.X * dirOut.X + dirOut.Y * dirOut.Y);
				if (len > 0)
					dirOut = {dirOut.X / len, dirOut.Y / len};
			}
			if (i == 0)
				dirIn = dirOut;
			if (i + 1 == n)
				dirOut = dirIn;
			P2 normIn{-dirIn.Y, dirIn.X}, normOut{-dirOut.Y, dirOut.X};
			P2 miter{normIn.X + normOut.X, normIn.Y + normOut.Y};
			auto mlen = std::sqrt(miter.X * miter.X + miter.Y * miter.Y);
			if (mlen < 1e-4f)
				miter = normOut;
			else
				miter = {miter.X / mlen, miter.Y / mlen};
			auto dot = miter.X * normOut.X + miter.Y * normOut.Y;
			auto scale = halfWidth / std::max(dot, 0.5f);
			left.push_back({pts[i].X + miter.X * scale, pts[i].Y + miter.Y * scale});
			right.push_back({pts[i].X - miter.X * scale, pts[i].Y - miter.Y * scale});
		}
		std::vector<P2> outline = left;
		for (auto it = right.rbegin(); it != right.rend(); ++it)
			outline.push_back(*it);
		return outline;
	}

	std::vector<P2> ConvexHull(std::vector<P2> pts)
	{
		std::sort(pts.begin(), pts.end(), [](const P2& a, const P2& b)
		{
			return a.X < b.X || (a.X == b.X && a.Y < b.Y);
		});
		if (pts.size() < 3)
			return pts;
		auto cross = [](const P2& o, const P2& a, const P2& b)
		{
			return (a.X - o.X) * (b.Y - o.Y) - (a.Y - o.Y) * (b.X - o.X);
		};
		std::vector<P2> hull(pts.size() * 2);
		size_t k = 0;
		for (size_t i = 0; i < pts.size(); i++)
		{
			while (k >= 2 && cross(hull[k - 2], hull[k - 1], pts[i]) <= 0)
				k--;
			hull[k++] = pts[i];
		}
		for (size_t i = pts.size() - 1, t = k + 1; i > 0; i--)
		{
			while (k >= t && cross(hull[k - 2], hull[k - 1], pts[i - 1]) <= 0)
				k--;
			hull[k++] = pts[i - 1];
		}
		hull.resize(k - 1);
		return hull;
	}

	std::vector<P2> CapsuleOutline(P2 a, float ra, P2 b, float rb, int segments = 24)
	{
		std::vector<P2> pts;
		for (int i = 0; i < segments; i++)
		{
			auto ang = 2.0f * 3.14159265f * i / segments;
			pts.push_back({a.X + std::cos(ang) * ra, a.Y + std::sin(ang) * ra});
			pts.push_back({b.X + std::cos(ang) * rb, b.Y + std::sin(ang) * rb});
		}
		return ConvexHull(pts);
	}

	std::vector<P2> Grow(const std::vector<P2>& poly, float amount)
	{
		// Pushes each point away from the centre; good enough for the small convex shapes it is used on.
		P2 c{0, 0};
		for (auto& p : poly)
		{
			c.X += p.X;
			c.Y += p.Y;
		}
		c.X /= poly.size();
		c.Y /= poly.size();
		std::vector<P2> out;
		for (auto& p : poly)
		{
			auto dx = p.X - c.X, dy = p.Y - c.Y;
			auto len = std::sqrt(dx * dx + dy * dy);
			if (len < 1e-5f)
				out.push_back(p);
			else
				out.push_back({p.X + dx / len * amount, p.Y + dy / len * amount});
		}
		return out;
	}

	std::vector<ArtVec> To3D(const std::vector<P2>& poly)
	{
		std::vector<ArtVec> out;
		for (auto& p : poly)
			out.push_back({p.X, p.Y, 0});
		return out;
	}

	std::vector<float> WallFloats(const std::vector<P2>& poly)
	{
		std::vector<float> f{600.0f, static_cast<float>(poly.size() + 1)};
		for (auto& p : poly)
		{
			f.push_back(p.X);
			f.push_back(p.Y);
		}
		f.push_back(poly[0].X);
		f.push_back(poly[0].Y);
		f.push_back(-1.0f);
		return f;
	}

	// ---------------------------------------------------------------------------------------------
	// Table builder

	struct Bounds
	{
		float MinX = 1e9f, MinY = 1e9f, MaxX = -1e9f, MaxY = -1e9f;

		void Add(float x, float y)
		{
			MinX = std::min(MinX, x);
			MinY = std::min(MinY, y);
			MaxX = std::max(MaxX, x);
			MaxY = std::max(MaxY, y);
		}
	};

	struct Texture
	{
		int Width = 0, Height = 0;
		std::vector<uint8_t> RGB;

		ArtColor Sample(float u, float v) const
		{
			if (Width == 0)
				return {0, 0, 0};
			u = std::min(std::max(u, 0.0f), Width - 1.001f);
			v = std::min(std::max(v, 0.0f), Height - 1.001f);
			auto x0 = static_cast<int>(u), y0 = static_cast<int>(v);
			auto fx = u - x0, fy = v - y0;
			auto px = [&](int x, int y, int c) { return RGB[(static_cast<size_t>(y) * Width + x) * 3 + c] / 255.0f; };
			ArtColor out;
			float* ch[3] = {&out.R, &out.G, &out.B};
			for (int c = 0; c < 3; c++)
			{
				auto top = px(x0, y0, c) * (1 - fx) + px(x0 + 1, y0, c) * fx;
				auto bot = px(x0, y0 + 1, c) * (1 - fx) + px(x0 + 1, y0 + 1, c) * fx;
				*ch[c] = top * (1 - fy) + bot * fy;
			}
			return out;
		}
	};

	class Builder
	{
	public:
		Builder(const Value& doc, const std::map<std::string, std::string>& files)
			: Doc(doc), Files(files)
		{
		}

		DatFile* Build(std::string& error);

	private:
		const Value& Doc;
		const std::map<std::string, std::string>& Files;
		DatFile* Dat = nullptr;
		TableCamera Camera;
		int Scale = 1;
		float W = 16, L = 29;
		float Rail = 0.7f, RailH = 1.0f, Apron = 1.6f;
		int TableW = 344;
		const int ScreenH = 416, ScreenW = 600;

		ArtColor Playfield, Playfield2, WallColor, Rubber, Accent, Accent2, Cabinet, PanelColor, PostColor;
		std::vector<std::pair<int, int>> Objects; // (type, group)
		int MatWall = -1, MatRubber = -1, MatFlipper = -1, MatMetal = -1;
		Texture PlayfieldTex;
		float TexPerUnit = 20;

		// Static scenery baked into the table bitmap.
		std::vector<std::function<void(Art3D&)>> Scenery;

		ArtColor Color(const Value& v, ArtColor def) const { return ArtColor::FromHex(v.Str(), def); }
		int Material(float smoothness, float elasticity);
		int Kicker(float threshold, float boost);
		void SetupCamera();
		Art3D NewCanvas(const Bounds& screen) const;
		Bounds ScreenBounds(const std::vector<ArtVec>& pts) const;
		void ResolveSprite(const Art3D& canvas, gdrv_bitmap8*& bmp, zmap_header_type*& zMap) const;

		bool AddWall(const Value& part, std::string& error);
		bool AddPost(const Value& part, std::string& error);
		bool AddSlingshot(const Value& part, std::string& error);
		bool AddBumper(const Value& part, std::string& error);
		bool AddFlipper(const Value& part, std::string& error);
		bool AddPlunger(const Value& part, std::string& error);
		bool AddDrain(const Value& part, std::string& error);
		bool AddLane(const Value& part, std::string& error);
		bool AddTarget(const Value& part, std::string& error);
		bool AddLight(const Value& part, std::string& error);
		bool AddDropTarget(const Value& part, std::string& error);
		bool AddKickout(const Value& part, std::string& error);
		bool AddSpinner(const Value& part, std::string& error);
		bool AddRamp(const Value& part, std::string& error);
		bool AddGate(const Value& part, std::string& error);
		bool ReadRules(std::string& error);
		std::map<std::string, int> Ids; // part id -> group

		void PaintPlayfieldTexture();
		int BuildTableLayer();
		int BuildBall();
		void BuildPanel(int& scoreGroup, int& ballGroup, int& playerGroup, int& infoGroup, int& missionGroup);
		int BuildDigits();
	};

	int Builder::Material(float smoothness, float elasticity)
	{
		GroupBuild g;
		g.ShortValue(300);
		g.Floats({301.0f, smoothness, 302.0f, elasticity});
		return Commit(Dat, g);
	}

	int Builder::Kicker(float threshold, float boost)
	{
		GroupBuild g;
		g.ShortValue(400);
		g.Floats({401.0f, threshold, 402.0f, boost});
		return Commit(Dat, g);
	}

	void Builder::SetupCamera()
	{
		// A fixed tilt and a camera distance that grows with the table, so every table gets the same look.
		auto distance = std::max(L * 0.93f, 20.0f);
		Camera.Setup(25.0f, distance, 1.0f, 0, 0);

		// Fit the whole cabinet into the table area of the screen.
		Bounds b;
		auto x0 = -W / 2 - Rail, x1 = W / 2 + Rail, y0 = -L / 2 - Rail, y1 = L / 2 + Apron;
		ArtVec corners[8] = {
			{x0, y0, 0}, {x1, y0, 0}, {x0, y1, 0}, {x1, y1, 0},
			{x0, y0, RailH}, {x1, y0, RailH}, {x0, y1, RailH}, {x1, y1, RailH}
		};
		for (auto& c : corners)
		{
			float sx, sy, depth;
			Camera.Project(c, sx, sy, depth);
			b.Add(sx, sy);
		}
		auto margin = 4.0f;
		auto focal = std::min((ScreenH - margin * 2) / (b.MaxY - b.MinY), (TableW - margin * 2) / (b.MaxX - b.MinX));
		auto usedH = (b.MaxY - b.MinY) * focal;
		auto centerX = TableW * 0.5f - focal * (b.MinX + b.MaxX) * 0.5f;
		auto centerY = margin + (ScreenH - margin * 2 - usedH) * 0.5f - focal * b.MinY;
		Camera.Setup(25.0f, distance, focal, centerX, centerY);
	}

	Bounds Builder::ScreenBounds(const std::vector<ArtVec>& pts) const
	{
		Bounds b;
		for (auto& p : pts)
		{
			float sx, sy, depth;
			Camera.Project(p, sx, sy, depth);
			b.Add(sx, sy);
		}
		return b;
	}

	Art3D Builder::NewCanvas(const Bounds& screen) const
	{
		auto x0 = static_cast<int>(std::floor(screen.MinX)) - 2;
		auto y0 = static_cast<int>(std::floor(screen.MinY)) - 2;
		auto x1 = static_cast<int>(std::ceil(screen.MaxX)) + 2;
		auto y1 = static_cast<int>(std::ceil(screen.MaxY)) + 2;
		x0 = std::max(0, x0);
		y0 = std::max(0, y0);
		x1 = std::min(TableW, x1);
		y1 = std::min(ScreenH, y1);
		return Art3D(Camera, x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0), Scale);
	}

	void Builder::ResolveSprite(const Art3D& canvas, gdrv_bitmap8*& bmp, zmap_header_type*& zMap) const
	{
		auto one = canvas.Resolve(canvas.SuperScale());
		ArtImage hd;
		if (Scale > 1)
			hd = canvas.Resolve(canvas.SS);
		MakeSprite(one, hd, canvas.X0, canvas.Y0, true, bmp, zMap);
	}

	// Common materials.
	ArtMaterial Plastic(ArtColor c)
	{
		ArtMaterial m;
		m.Color = c;
		m.Specular = 0.45f;
		m.Shininess = 40;
		return m;
	}

	ArtMaterial Metal(ArtColor c)
	{
		ArtMaterial m;
		m.Color = c;
		m.Ambient = 0.3f;
		m.Diffuse = 0.6f;
		m.Specular = 0.8f;
		m.Shininess = 60;
		return m;
	}

	ArtMaterial RubberMat(ArtColor c)
	{
		ArtMaterial m;
		m.Color = c;
		m.Specular = 0.12f;
		m.Shininess = 10;
		return m;
	}

	bool Builder::AddWall(const Value& part, std::string& error)
	{
		auto pts = ReadPoints(part["points"]);
		if (pts.size() < 2)
		{
			error = "A wall needs at least 2 points";
			return false;
		}
		auto closed = part["closed"].Bool(false);
		auto thickness = part["thickness"].Float(0.3f);
		auto height = part["height"].Float(0.55f);
		auto color = Color(part["color"], WallColor);

		std::vector<P2> outline = closed && pts.size() >= 3 ? pts : ThickLine(pts, thickness * 0.5f);
		outline = Outward(outline);

		GroupBuild g;
		g.ShortValue(200);
		g.Shorts({300, part["bounce"].IsNumber() ? Material(0.9f, part["bounce"].Float()) : MatWall});
		g.Floats(WallFloats(outline));
		auto group = Commit(Dat, g);
		Objects.push_back({1000, group});
		if (part["score"].Int() > 0)
			customtable::Scores[group] = part["score"].Int();

		// Art: segments as blocks with round joints, so curves look smooth.
		auto mat = Plastic(color);
		auto topMat = Plastic(color.Mix({1, 1, 1}, 0.25f));
		auto half = thickness * 0.5f;
		Scenery.push_back([=](Art3D& art)
		{
			if (closed && pts.size() >= 3)
			{
				art.Prism(To3D(outline), 0, height, ArtSolid(topMat), ArtSolid(mat));
				return;
			}
			for (size_t i = 0; i + 1 < pts.size(); i++)
			{
				auto seg = ThickLine({pts[i], pts[i + 1]}, half);
				art.Prism(To3D(seg), 0, height, ArtSolid(topMat), ArtSolid(mat));
			}
			for (auto& p : pts)
				art.Cylinder(p.X, p.Y, half, 0, height, ArtSolid(topMat), ArtSolid(mat), 20);
		});
		return true;
	}

	bool Builder::AddPost(const Value& part, std::string& error)
	{
		auto x = part["x"].Float(), y = part["y"].Float();
		auto radius = part["radius"].Float(0.2f);
		GroupBuild g;
		g.ShortValue(200);
		g.Shorts({300, MatRubber});
		g.Floats({600.0f, 1.0f, x, y, radius, -1.0f});
		auto group = Commit(Dat, g);
		Objects.push_back({1000, group});
		if (part["score"].Int() > 0)
			customtable::Scores[group] = part["score"].Int();

		auto rubber = RubberMat(Rubber);
		auto metal = Metal(PostColor);
		Scenery.push_back([=](Art3D& art)
		{
			art.Cylinder(x, y, radius, 0.05f, 0.35f, ArtSolid(rubber), ArtSolid(rubber), 32);
			art.Cylinder(x, y, radius * 0.55f, 0, 0.62f, ArtSolid(metal), ArtSolid(metal), 24);
		});
		return true;
	}

	bool Builder::AddSlingshot(const Value& part, std::string& error)
	{
		auto pts = ReadPoints(part["points"]);
		if (pts.size() < 3)
		{
			error = "A slingshot needs 3 corner points";
			return false;
		}
		pts = Outward(pts);
		auto strength = part["strength"].Float(1.0f);
		GroupBuild g;
		g.ShortValue(200);
		g.Shorts({100, 1, 300, MatRubber, 400, Kicker(1.0f, 16.0f * strength)});
		g.Floats(WallFloats(pts));

		auto color = Color(part["color"], Accent);
		auto body = Plastic(color);
		auto top = Plastic(color.Mix({1, 1, 1}, 0.2f));
		auto rubber = RubberMat(Rubber);
		auto inner = Grow(pts, -0.12f);
		auto band = Grow(pts, 0.03f);
		Scenery.push_back([=](Art3D& art)
		{
			art.Prism(To3D(band), 0.05f, 0.3f, ArtSolid(rubber), ArtSolid(rubber));
			art.Prism(To3D(inner), 0, 0.5f, ArtSolid(top), ArtSolid(body));
		});

		// Flash when kicking: lit rubber and plastic.
		std::vector<ArtVec> corners;
		for (auto& p : band)
		{
			corners.push_back({p.X, p.Y, 0});
			corners.push_back({p.X, p.Y, 0.5f});
		}
		auto canvas = NewCanvas(ScreenBounds(corners));
		auto litRubber = rubber;
		litRubber.Glow = 0.6f;
		litRubber.Color = Rubber.Mix(color, 0.35f);
		auto litBody = top;
		litBody.Glow = 0.9f;
		canvas.Prism(To3D(band), 0.05f, 0.3f, ArtSolid(litRubber), ArtSolid(litRubber));
		canvas.Prism(To3D(inner), 0, 0.5f, ArtSolid(litBody), ArtSolid(litBody));
		gdrv_bitmap8* bmp;
		zmap_header_type* zMap;
		ResolveSprite(canvas, bmp, zMap);
		g.Sprite(bmp, zMap);

		auto group = Commit(Dat, g);
		Objects.push_back({1000, group});
		customtable::Scores[group] = part["score"].Int(10);
		return true;
	}


	// Parts with several images (bumpers, flippers, the plunger) store the extra images in records that follow
	// the part's own record.
	struct StatefulPart
	{
		GroupBuild Main;
		std::vector<std::pair<gdrv_bitmap8*, zmap_header_type*>> States;
	};

	int CommitStateful(DatFile* dat, StatefulPart& part)
	{
		auto first = Commit(dat, part.Main);
		for (auto& state : part.States)
		{
			GroupBuild g;
			g.ShortValue(201);
			g.Sprite(state.first, state.second);
			Commit(dat, g);
		}
		return first;
	}

	bool Builder::AddBumper(const Value& part, std::string& error)
	{
		auto x = part["x"].Float(), y = part["y"].Float();
		auto r = part["radius"].Float(0.55f);
		auto strength = part["strength"].Float(1.0f);
		auto color = Color(part["color"], Accent);
		auto capColor = Color(part["capColor"], Accent2);

		StatefulPart sp;
		sp.Main.ShortValue(200);
		sp.Main.Shorts({100, 2, 400, Kicker(2.5f, 15.0f * strength)});
		sp.Main.Floats({600.0f, 1.0f, x, y, r, -1.0f});
		sp.Main.Floats({407.0f, 0.1f});

		std::vector<ArtVec> corners;
		for (int i = 0; i < 16; i++)
		{
			auto a = 2.0f * 3.14159265f * i / 16;
			corners.push_back({x + std::cos(a) * r * 1.25f, y + std::sin(a) * r * 1.25f, 0});
			corners.push_back({x + std::cos(a) * r * 1.1f, y + std::sin(a) * r * 1.1f, 0.95f});
		}
		auto bounds = ScreenBounds(corners);

		for (int lit = 0; lit < 2; lit++)
		{
			auto canvas = NewCanvas(bounds);
			auto skirt = Plastic(color.Scaled(0.6f));
			auto body = Plastic(color);
			auto ring = Metal(PostColor);
			auto cap = Plastic(capColor);
			auto capTop = Plastic(capColor.Mix({1, 1, 1}, 0.3f));
			if (lit)
			{
				body.Glow = 0.45f;
				body.Color = color.Mix({1, 1, 1}, 0.2f);
				cap.Glow = 0.9f;
				capTop.Glow = 1.0f;
				capTop.Color = capColor.Mix({1, 1, 1}, 0.55f);
			}
			canvas.Cone(x, y, r * 1.22f, r * 1.0f, 0, 0.12f, ArtSolid(skirt));
			canvas.Cylinder(x, y, r * 0.86f, 0.1f, 0.62f, ArtSolid(body), ArtSolid(body));
			canvas.Cylinder(x, y, r * 1.0f, 0.62f, 0.7f, ArtSolid(ring), ArtSolid(ring));
			canvas.Cylinder(x, y, r * 0.95f, 0.7f, 0.86f, ArtSolid(capTop), ArtSolid(cap));
			canvas.Disc(x, y, r * 0.32f, 0.865f, ArtSolid(Metal({0.92f, 0.92f, 0.96f})));
			gdrv_bitmap8* bmp;
			zmap_header_type* zMap;
			ResolveSprite(canvas, bmp, zMap);
			if (lit == 0)
				sp.Main.Sprite(bmp, zMap);
			else
				sp.States.push_back({bmp, zMap});
		}
		auto group = CommitStateful(Dat, sp);
		Objects.push_back({1005, group});
		customtable::Scores[group] = part["score"].Int(100);
		return true;
	}

	bool Builder::AddFlipper(const Value& part, std::string& error)
	{
		auto side = part["side"].Str("left");
		auto left = side != "right";
		auto px = part["x"].Float(), py = part["y"].Float();
		auto length = part["length"].Float(2.0f);
		auto restAngle = part["angle"].Float(30.0f);
		auto swing = part["swing"].Float(55.0f);
		auto rb = part["baseRadius"].Float(0.3f), rt = part["tipRadius"].Float(0.15f);
		auto strength = part["strength"].Float(1.0f);
		auto color = Color(part["color"], {0.95f, 0.95f, 0.97f});
		auto rubberColor = Color(part["rubberColor"], Accent);

		auto dir = [&](float deg)
		{
			auto a = deg * 3.14159265f / 180.0f;
			return P2{(left ? 1.0f : -1.0f) * std::cos(a), std::sin(a)};
		};
		auto restDir = dir(restAngle), upDir = dir(restAngle - swing);
		P2 pivot{px, py};
		P2 tipRest{px + restDir.X * length, py + restDir.Y * length};
		P2 tipUp{px + upDir.X * length, py + upDir.Y * length};

		StatefulPart sp;
		const int frames = 8;
		sp.Main.ShortValue(200);
		sp.Main.Shorts({100, frames, 300, MatFlipper});
		sp.Main.Floats({800.0f, pivot.X, pivot.Y, rb, -1.0f});
		sp.Main.Floats({801.0f, tipRest.X, tipRest.Y, rt, -1.0f});
		sp.Main.Floats({802.0f, tipUp.X, tipUp.Y, rt, -1.0f});
		sp.Main.Floats({803.0f, 0.9f * strength});
		sp.Main.Floats({804.0f, 0.04f / std::max(0.3f, strength)});
		sp.Main.Floats({805.0f, 0.08f});

		std::vector<std::vector<P2>> shapes;
		std::vector<ArtVec> corners;
		for (int i = 0; i < frames; i++)
		{
			auto d = dir(restAngle - swing * i / (frames - 1));
			P2 tip{px + d.X * length, py + d.Y * length};
			auto hull = CapsuleOutline(pivot, rb + 0.03f, tip, rt + 0.03f);
			shapes.push_back(hull);
			for (auto& p : hull)
			{
				corners.push_back({p.X, p.Y, 0});
				corners.push_back({p.X, p.Y, 0.5f});
			}
		}
		auto bounds = ScreenBounds(corners);
		auto body = Plastic(color);
		auto bodyTop = Plastic(color.Mix({1, 1, 1}, 0.3f));
		auto rubber = RubberMat(rubberColor);
		auto cap = Metal(PostColor);
		for (int i = 0; i < frames; i++)
		{
			auto canvas = NewCanvas(bounds);
			auto d = dir(restAngle - swing * i / (frames - 1));
			P2 tip{px + d.X * length, py + d.Y * length};
			auto inner = CapsuleOutline(pivot, rb - 0.02f, tip, rt - 0.02f);
			canvas.Prism(To3D(shapes[i]), 0.06f, 0.3f, ArtSolid(rubber), ArtSolid(rubber), true);
			canvas.Prism(To3D(inner), 0, 0.45f, ArtSolid(bodyTop), ArtSolid(body), true);
			canvas.Cylinder(pivot.X, pivot.Y, rb * 0.35f, 0.45f, 0.5f, ArtSolid(cap), ArtSolid(cap), 20);
			gdrv_bitmap8* bmp;
			zmap_header_type* zMap;
			ResolveSprite(canvas, bmp, zMap);
			if (i == 0)
				sp.Main.Sprite(bmp, zMap);
			else
				sp.States.push_back({bmp, zMap});
		}
		auto group = CommitStateful(Dat, sp);
		Objects.push_back({left ? 1003 : 1004, group});
		return true;
	}

	bool Builder::AddPlunger(const Value& part, std::string& error)
	{
		auto x = part["x"].Float(), y = part["y"].Float();
		auto width = part["width"].Float(0.7f);
		auto travel = part["travel"].Float(1.0f);
		auto color = Color(part["color"], Accent);

		StatefulPart sp;
		const int frames = 8;
		sp.Main.ShortValue(200);
		sp.Main.Shorts({100, frames});
		sp.Main.Floats({600.0f, 2.0f, x - width * 0.5f, y, x + width * 0.5f, y, -1.0f});
		sp.Main.Floats({601.0f, x, y - 1.2f, 0, -1.0f});

		auto rodEnd = L / 2 + Apron * 0.8f;
		std::vector<ArtVec> corners{
			{x - width, y - 0.1f, 0}, {x + width, y - 0.1f, 0}, {x - width, rodEnd + travel, 0},
			{x + width, rodEnd + travel, 0}, {x - width, y - 0.1f, 0.6f}, {x + width, y - 0.1f, 0.6f}
		};
		auto bounds = ScreenBounds(corners);
		auto head = Plastic(color);
		auto headTop = Plastic(color.Mix({1, 1, 1}, 0.25f));
		auto rod = Metal(PostColor);
		auto spring = Metal(PostColor.Scaled(0.8f));
		for (int i = 0; i < frames; i++)
		{
			auto canvas = NewCanvas(bounds);
			auto off = travel * i / (frames - 1);
			auto hy = y + off;
			std::vector<ArtVec> headBox{
				{x - width * 0.42f, hy, 0}, {x + width * 0.42f, hy, 0}, {x + width * 0.42f, hy + 0.35f, 0},
				{x - width * 0.42f, hy + 0.35f, 0}
			};
			canvas.Prism(headBox, 0, 0.5f, ArtSolid(headTop), ArtSolid(head));
			std::vector<ArtVec> rodBox{
				{x - 0.07f, hy + 0.35f, 0}, {x + 0.07f, hy + 0.35f, 0}, {x + 0.07f, rodEnd + off, 0},
				{x - 0.07f, rodEnd + off, 0}
			};
			canvas.Prism(rodBox, 0.15f, 0.3f, ArtSolid(rod), ArtSolid(rod));
			// Spring coils between the head and the end of the lane, squeezed as it is pulled.
			auto coils = 7;
			auto springStart = hy + 0.45f, springEnd = rodEnd - 0.2f;
			for (int c = 0; c < coils; c++)
			{
				auto cy = springStart + (springEnd - springStart) * c / (coils - 1);
				std::vector<ArtVec> coil{
					{x - width * 0.3f, cy, 0}, {x + width * 0.3f, cy, 0}, {x + width * 0.3f, cy + 0.06f, 0},
					{x - width * 0.3f, cy + 0.06f, 0}
				};
				canvas.Prism(coil, 0.05f, 0.4f, ArtSolid(spring), ArtSolid(spring));
			}
			gdrv_bitmap8* bmp;
			zmap_header_type* zMap;
			ResolveSprite(canvas, bmp, zMap);
			if (i == 0)
				sp.Main.Sprite(bmp, zMap);
			else
				sp.States.push_back({bmp, zMap});
		}
		auto group = CommitStateful(Dat, sp);
		Objects.push_back({1001, group});
		return true;
	}

	bool Builder::AddDrain(const Value& part, std::string& error)
	{
		auto y = part["y"].Float(L / 2 - 0.3f);
		auto x0 = part["x0"].Float(-W / 2), x1 = part["x1"].Float(W / 2);
		if (x0 > x1)
			std::swap(x0, x1);
		GroupBuild g;
		g.ShortValue(200);
		g.Name("drain");
		g.Floats({600.0f, 2.0f, x0, y, x1, y, -1.0f});
		g.Floats({407.0f, 1.5f});
		Objects.push_back({1007, Commit(Dat, g)});
		return true;
	}


	bool Builder::AddLane(const Value& part, std::string& error)
	{
		// A rollover switch across a lane: the ball rolls over it and it counts a hit.
		auto pts = ReadPoints(part["points"]);
		if (pts.size() != 2)
		{
			error = "A lane needs 2 points (the line the ball crosses)";
			return false;
		}
		// A thin closed shape, so the switch counts the ball from either side (each edge only faces one way).
		GroupBuild g;
		g.ShortValue(200);
		g.Floats(WallFloats(Outward(ThickLine(pts, 0.02f))));
		auto group = Commit(Dat, g);
		Objects.push_back({1024, group});
		customtable::Scores[group] = part["score"].Int(100);
		customtable::Bonus[group] = part["bonus"].Int(500);

		auto a = pts[0], b = pts[1];
		auto wire = Metal(PostColor);
		Scenery.push_back([=](Art3D& art)
		{
			// Wire switch: a thin arc of metal across the middle of the line.
			P2 mid{(a.X + b.X) * 0.5f, (a.Y + b.Y) * 0.5f};
			P2 d{b.X - a.X, b.Y - a.Y};
			auto len = std::sqrt(d.X * d.X + d.Y * d.Y);
			if (len < 0.01f)
				return;
			P2 n{d.X / len, d.Y / len};
			auto half = std::min(0.35f, len * 0.3f);
			auto seg = ThickLine({{mid.X - n.X * half, mid.Y - n.Y * half}, {mid.X + n.X * half, mid.Y + n.Y * half}},
			                     0.03f);
			art.Prism(To3D(seg), 0, 0.06f, ArtSolid(wire), ArtSolid(wire));
		});
		return true;
	}

	bool Builder::AddTarget(const Value& part, std::string& error)
	{
		// A stand-up target: a plastic face on a post. The face looks along "angle" (degrees, 0 = right,
		// 90 = towards the player, -90 = up the table).
		auto x = part["x"].Float(), y = part["y"].Float();
		auto width = part["width"].Float(0.8f);
		auto angle = part["angle"].Float(90.0f) * 3.14159265f / 180.0f;
		auto color = Color(part["color"], Accent);
		P2 face{std::cos(angle), std::sin(angle)};
		P2 along{-face.Y, face.X};
		const float depth = 0.16f;
		auto corner = [&](float s, float t) { return P2{x + along.X * s + face.X * t, y + along.Y * s + face.Y * t}; };
		std::vector<P2> box{
			corner(-width / 2, -depth / 2), corner(width / 2, -depth / 2), corner(width / 2, depth / 2),
			corner(-width / 2, depth / 2)
		};
		box = Outward(box);

		StatefulPart sp;
		sp.Main.ShortValue(200);
		sp.Main.Shorts({100, 2, 300, MatRubber, 400, Kicker(0.6f, part["strength"].Float(2.0f))});
		sp.Main.Floats(WallFloats(box));

		std::vector<ArtVec> corners;
		for (auto& p : box)
		{
			corners.push_back({p.X, p.Y, 0});
			corners.push_back({p.X, p.Y, 0.65f});
		}
		auto bounds = ScreenBounds(corners);
		auto post = Metal(PostColor.Scaled(0.8f));
		auto inner = Grow(box, -0.02f);
		std::vector<P2> stem{
			corner(-0.06f, -depth / 2 - 0.12f), corner(0.06f, -depth / 2 - 0.12f), corner(0.06f, -depth / 2),
			corner(-0.06f, -depth / 2)
		};
		stem = Outward(stem);
		for (int hit = 0; hit < 2; hit++)
		{
			auto canvas = NewCanvas(bounds);
			auto body = Plastic(color);
			auto top = Plastic(color.Mix({1, 1, 1}, 0.25f));
			if (hit)
			{
				body.Glow = 0.8f;
				top.Glow = 0.9f;
				body.Color = color.Mix({1, 1, 1}, 0.3f);
			}
			canvas.Prism(To3D(stem), 0, 0.4f, ArtSolid(post), ArtSolid(post));
			canvas.Prism(To3D(inner), 0.08f, 0.6f, ArtSolid(top), ArtSolid(body));
			gdrv_bitmap8* bmp;
			zmap_header_type* zMap;
			ResolveSprite(canvas, bmp, zMap);
			if (hit == 0)
				sp.Main.Sprite(bmp, zMap);
			else
				sp.States.push_back({bmp, zMap});
		}
		auto group = CommitStateful(Dat, sp);
		Objects.push_back({1019, group});
		customtable::Scores[group] = part["score"].Int(250);
		customtable::Bonus[group] = part["bonus"].Int(500);
		return true;
	}

	bool Builder::AddLight(const Value& part, std::string& error)
	{
		// A lamp under a playfield insert. The unlit insert is painted on the playfield; this is the lit image.
		auto x = part["x"].Float(), y = part["y"].Float(), r = part["radius"].Float(0.35f);
		auto color = Color(part["color"], Accent);
		auto arrow = part["shape"].Str() == "arrow";
		auto a = part["angle"].Float(-90.0f) * 3.14159265f / 180.0f;
		P2 dir{std::cos(a), std::sin(a)};

		std::vector<ArtVec> corners{{x - r * 1.6f, y - r * 1.6f, 0}, {x + r * 1.6f, y + r * 1.6f, 0},
		                            {x - r * 1.6f, y + r * 1.6f, 0}, {x + r * 1.6f, y - r * 1.6f, 0}};
		auto canvas = NewCanvas(ScreenBounds(corners));
		// A lit LED insert keeps its colour: full brightness, a hot white-ish core in the middle and a
		// small shine. (Overdriving the colour would wash every lamp out to white.)
		auto peak = std::max(0.05f, std::max(color.R, std::max(color.G, color.B)));
		ArtColor bright{color.R / peak, color.G / peak, color.B / peak};
		auto lamp = [bright, x, y, r, arrow](const ArtVec& pos, const ArtVec& n) -> ArtColor
		{
			auto dx = pos.X - x, dy = pos.Y - y;
			auto d = std::min(1.0f, std::sqrt(dx * dx + dy * dy) / (arrow ? r * 1.3f : r));
			auto core = (1.0f - d) * (1.0f - d) * (arrow ? 0.35f : 0.6f);
			auto c = bright.Scaled(0.82f + 0.18f * (1.0f - d)).Mix({1, 1, 1}, core);
			ArtMaterial shine;
			shine.Color = {0, 0, 0};
			shine.Specular = 0.35f;
			shine.Shininess = 30;
			auto spec = ArtShade(shine, pos, n);
			return {std::min(1.0f, c.R + spec.R), std::min(1.0f, c.G + spec.G), std::min(1.0f, c.B + spec.B)};
		};
		if (arrow)
		{
			P2 tip{x + dir.X * r * 1.3f, y + dir.Y * r * 1.3f};
			for (int side = -1; side <= 1; side += 2)
			{
				P2 b{x - dir.X * r * 0.6f - dir.Y * r * side, y - dir.Y * r * 0.6f + dir.X * r * side};
				canvas.Prism(To3D(ThickLine({tip, b}, r * 0.27f)), 0, 0.012f, lamp, lamp);
				canvas.Cylinder(b.X, b.Y, r * 0.27f, 0, 0.012f, lamp, lamp, 16);
			}
			canvas.Cylinder(tip.X, tip.Y, r * 0.27f, 0, 0.012f, lamp, lamp, 16);
		}
		else
			canvas.Cylinder(x, y, r, 0, 0.012f, lamp, lamp, 32);
		gdrv_bitmap8* bmp;
		zmap_header_type* zMap;
		ResolveSprite(canvas, bmp, zMap);

		GroupBuild g;
		g.ShortValue(200);
		g.Shorts({100, 1});
		g.Sprite(bmp, zMap);
		g.Floats({900.0f, part["flash"].Float(0.15f)});
		g.Floats({901.0f, part["flash"].Float(0.15f)});
		Objects.push_back({1002, Commit(Dat, g)});
		return true;
	}

	bool Builder::AddDropTarget(const Value& part, std::string& error)
	{
		// A drop target: drops into the playfield when hit (the ball then rolls over it) and pops back up when
		// its group is complete. Faces along "angle" like a stand-up target.
		auto x = part["x"].Float(), y = part["y"].Float();
		auto width = part["width"].Float(0.8f);
		auto angle = part["angle"].Float(90.0f) * 3.14159265f / 180.0f;
		auto color = Color(part["color"], Accent);
		P2 face{std::cos(angle), std::sin(angle)};
		P2 along{-face.Y, face.X};
		const float depth = 0.2f;
		auto corner = [&](float s, float t) { return P2{x + along.X * s + face.X * t, y + along.Y * s + face.Y * t}; };
		std::vector<P2> box{
			corner(-width / 2, -depth / 2), corner(width / 2, -depth / 2), corner(width / 2, depth / 2),
			corner(-width / 2, depth / 2)
		};
		box = Outward(box);

		GroupBuild g;
		g.ShortValue(200);
		g.Shorts({100, 1, 300, MatRubber, 400, Kicker(0.6f, part["strength"].Float(1.5f))});
		g.Floats(WallFloats(box));
		g.Floats({407.0f, part["popup"].Float(1.0f)});

		std::vector<ArtVec> corners;
		for (auto& p : box)
		{
			corners.push_back({p.X, p.Y, 0});
			corners.push_back({p.X, p.Y, 0.75f});
		}
		auto canvas = NewCanvas(ScreenBounds(corners));
		auto body = Plastic(color);
		auto top = Plastic(color.Mix({1, 1, 1}, 0.3f));
		canvas.Prism(To3D(Grow(box, -0.01f)), 0, 0.7f, ArtSolid(top), ArtSolid(body));
		// A white stripe across the face.
		std::vector<P2> stripe{corner(-width / 2 + 0.06f, depth / 2), corner(width / 2 - 0.06f, depth / 2),
		                       corner(width / 2 - 0.06f, depth / 2 + 0.005f), corner(-width / 2 + 0.06f, depth / 2 + 0.005f)};
		auto white = Plastic({0.95f, 0.95f, 0.98f});
		canvas.Prism(To3D(Outward(stripe)), 0.42f, 0.5f, ArtSolid(white), ArtSolid(white));
		gdrv_bitmap8* bmp;
		zmap_header_type* zMap;
		ResolveSprite(canvas, bmp, zMap);
		g.Sprite(bmp, zMap);
		auto group = Commit(Dat, g);
		Objects.push_back({1006, group});
		customtable::Scores[group] = part["score"].Int(500);
		customtable::Bonus[group] = part["bonus"].Int(1000);
		return true;
	}

	bool Builder::AddKickout(const Value& part, std::string& error)
	{
		// A hole that catches the ball, holds it for a moment and kicks it out along "angle".
		auto x = part["x"].Float(), y = part["y"].Float();
		auto r = part["radius"].Float(0.45f);
		auto angle = part["angle"].Float(90.0f) * 3.14159265f / 180.0f;
		auto strength = part["strength"].Float(1.0f);

		GroupBuild kick;
		kick.ShortValue(400);
		kick.Floats({401.0f, 1e9f, 402.0f, 14.0f * strength, 403.0f, 10.0f, 404.0f, std::cos(angle), std::sin(angle),
		             0.0f, 405.0f, 0.12f});
		auto kicker = Commit(Dat, kick);

		GroupBuild g;
		g.ShortValue(200);
		g.Shorts({400, kicker});
		g.Floats({600.0f, 1.0f, x, y, r, -1.0f});
		g.Floats({305.0f, 18.0f});
		g.Floats({306.0f, 0.3f});
		g.Floats({408.0f, 0.1f, 0.1f, 0.12f, 0.12f});
		auto group = Commit(Dat, g);
		Objects.push_back({1012, group});
		customtable::Scores[group] = part["score"].Int(2500);
		customtable::Bonus[group] = part["bonus"].Int(1000);

		// Art: a dark hole (painted on the playfield) with a metal rim and a little back wall.
		auto rim = Metal(PostColor);
		auto back = Plastic(Color(part["color"], Accent));
		Scenery.push_back([=](Art3D& art)
		{
			std::vector<ArtVec> ring;
			const int n = 28;
			for (int i = 0; i <= n; i++)
			{
				// Half ring at the back of the hole, opposite the kick direction.
				auto a = angle + 3.14159265f * 0.5f + 3.14159265f * i / n;
				auto a2 = angle + 3.14159265f * 0.5f + 3.14159265f * (i + 1) / n;
				if (i == n)
					break;
				P2 p0{x + std::cos(a) * r * 1.05f, y + std::sin(a) * r * 1.05f};
				P2 p1{x + std::cos(a2) * r * 1.05f, y + std::sin(a2) * r * 1.05f};
				art.Prism(To3D(Outward(ThickLine({p0, p1}, 0.06f))), 0, 0.35f, ArtSolid(back), ArtSolid(back));
			}
			for (int i = 0; i < n; i++)
			{
				auto a = 2 * 3.14159265f * i / n, a2 = 2 * 3.14159265f * (i + 1) / n;
				P2 p0{x + std::cos(a) * r, y + std::sin(a) * r}, p1{x + std::cos(a2) * r, y + std::sin(a2) * r};
				art.Prism(To3D(Outward(ThickLine({p0, p1}, 0.035f))), 0, 0.05f, ArtSolid(rim), ArtSolid(rim));
			}
		});
		return true;
	}

	bool Builder::AddSpinner(const Value& part, std::string& error)
	{
		// A flag on a wire across a lane: the ball spins it as it goes through, and every turn scores.
		auto x = part["x"].Float(), y = part["y"].Float();
		auto width = part["width"].Float(1.0f);
		auto angle = part["angle"].Float(0.0f) * 3.14159265f / 180.0f; // direction of the wire
		auto color = Color(part["color"], Accent2);
		P2 d{std::cos(angle), std::sin(angle)};
		P2 n{-d.Y, d.X};
		P2 a{x - d.X * width / 2, y - d.Y * width / 2}, b{x + d.X * width / 2, y + d.Y * width / 2};
		const float axis = 0.62f, half = 0.32f;
		const int frames = 8;

		StatefulPart sp;
		sp.Main.ShortValue(200);
		sp.Main.Shorts({100, frames});
		sp.Main.Floats({600.0f, 2.0f, a.X, a.Y, b.X, b.Y, -1.0f});
		sp.Main.Floats({1200.0f, 50000.0f});
		sp.Main.Floats({1201.0f, 5.0f});
		sp.Main.Floats({1202.0f, part["friction"].Float(0.75f)});

		std::vector<ArtVec> corners{{a.X + n.X * half, a.Y + n.Y * half, 0}, {b.X - n.X * half, b.Y - n.Y * half, 0},
		                            {a.X - n.X * half, a.Y - n.Y * half, axis + half}, {b.X + n.X * half, b.Y + n.Y * half, axis + half}};
		auto bounds = ScreenBounds(corners);
		auto plate = Metal(color.Mix({1, 1, 1}, 0.15f));
		auto plateBack = Metal(color.Scaled(0.7f));
		for (int f = 0; f < frames; f++)
		{
			auto t = 3.14159265f * f / frames;
			auto c = std::cos(t), s2 = std::sin(t);
			// The plate hangs from the wire; at frame 0 it hangs straight down.
			auto px = [&](P2 p, float k) { return ArtVec{p.X + n.X * s2 * half * k, p.Y + n.Y * s2 * half * k, axis - c * half * k}; };
			auto canvas = NewCanvas(bounds);
			auto ia = P2{a.X + d.X * 0.06f, a.Y + d.Y * 0.06f}, ib = P2{b.X - d.X * 0.06f, b.Y - d.Y * 0.06f};
			canvas.Quad(px(ia, 0), px(ib, 0), px(ib, 1), px(ia, 1), [=](const ArtVec& pos, const ArtVec& nv)
			{
				return ArtShade(nv.Z >= 0 ? plate : plateBack, pos, nv);
			});
			gdrv_bitmap8* bmp;
			zmap_header_type* zMap;
			ResolveSprite(canvas, bmp, zMap);
			if (f == 0)
				sp.Main.Sprite(bmp, zMap);
			else
				sp.States.push_back({bmp, zMap});
		}
		auto group = CommitStateful(Dat, sp);
		Objects.push_back({1018, group});
		customtable::Scores[group] = part["score"].Int(100);
		customtable::Bonus[group] = part["bonus"].Int(0);

		// Posts and wire on the table.
		auto post = Metal(PostColor);
		Scenery.push_back([=](Art3D& art)
		{
			art.Cylinder(a.X, a.Y, 0.06f, 0, axis + 0.08f, ArtSolid(post), ArtSolid(post), 12);
			art.Cylinder(b.X, b.Y, 0.06f, 0, axis + 0.08f, ArtSolid(post), ArtSolid(post), 12);
		});
		return true;
	}

	bool Builder::AddRamp(const Value& part, std::string& error)
	{
		// A ramp drawn as a path: a raised lane with rails on both sides. The ball counts a ramp shot when
		// it leaves the far end. (The ball stays at playfield height; the ramp floor is just above it.)
		auto pts = ReadPoints(part["points"]);
		if (pts.size() < 2)
		{
			error = "A ramp needs at least 2 points (its path)";
			return false;
		}
		auto width = part["width"].Float(1.1f);
		auto color = Color(part["color"], Accent2);
		auto railColor = Color(part["railColor"], PostColor);

		// Rails: the two sides of the path.
		auto outline = ThickLine(pts, width * 0.5f);
		auto count = pts.size();
		std::vector<P2> left(outline.begin(), outline.begin() + count);
		std::vector<P2> right(outline.begin() + count, outline.end());
		std::reverse(right.begin(), right.end());
		for (auto side : {&left, &right})
		{
			GroupBuild g;
			g.ShortValue(200);
			g.Shorts({300, MatWall});
			g.Floats(WallFloats(Outward(ThickLine(*side, 0.06f))));
			Objects.push_back({1000, Commit(Dat, g)});
		}

		// A one-way flap across the far end lets the ball out but stops balls rolling in backwards.
		auto exitA = left.back(), exitB = right.back();
		P2 dir{pts[count - 1].X - pts[count - 2].X, pts[count - 1].Y - pts[count - 2].Y};
		auto dl = std::sqrt(dir.X * dir.X + dir.Y * dir.Y);
		if (dl > 0)
			dir = {dir.X / dl, dir.Y / dl};
		{
			// The gate passes balls coming from the right-hand side of its first-to-second point line.
			P2 ga = exitA, gb = exitB;
			if ((gb.X - ga.X) * -dir.Y - (gb.Y - ga.Y) * -dir.X < 0)
				std::swap(ga, gb);
			GroupBuild gate;
			gate.ShortValue(200);
			gate.Shorts({300, MatWall});
			gate.Floats({600.0f, 2.0f, ga.X, ga.Y, gb.X, gb.Y, -1.0f});
			Objects.push_back({1016, Commit(Dat, gate)});
		}

		// Exit switch just before the far end.
		P2 inA{exitA.X - dir.X * 0.4f, exitA.Y - dir.Y * 0.4f}, inB{exitB.X - dir.X * 0.4f, exitB.Y - dir.Y * 0.4f};
		GroupBuild sw;
		sw.ShortValue(200);
		sw.Floats(WallFloats(Outward(ThickLine({inA, inB}, 0.02f))));
		auto group = Commit(Dat, sw);
		Objects.push_back({1024, group});
		customtable::Scores[group] = part["score"].Int(5000);
		customtable::Bonus[group] = part["bonus"].Int(2000);

		auto floor = Plastic(color.Scaled(0.3f));
		floor.Ambient = 0.6f;
		auto chevron = Plastic(color);
		chevron.Glow = 0.5f;
		auto rail = Metal(railColor);
		Scenery.push_back([=](Art3D& art)
		{
			// Floor in segments, with chevrons pointing up the ramp.
			for (size_t i = 0; i + 1 < count; i++)
			{
				art.Quad({left[i].X, left[i].Y, 0.02f}, {left[i + 1].X, left[i + 1].Y, 0.02f},
				         {right[i + 1].X, right[i + 1].Y, 0.02f}, {right[i].X, right[i].Y, 0.02f}, ArtSolid(floor));
				P2 d{pts[i + 1].X - pts[i].X, pts[i + 1].Y - pts[i].Y};
				auto len = std::sqrt(d.X * d.X + d.Y * d.Y);
				if (len < 0.01f)
					continue;
				d = {d.X / len, d.Y / len};
				P2 n{-d.Y, d.X};
				auto half = width * 0.3f;
				for (float t = 0.6f; t + 0.3f < len; t += 1.2f)
				{
					P2 tip{pts[i].X + d.X * t, pts[i].Y + d.Y * t};
					for (int s = -1; s <= 1; s += 2)
					{
						P2 wing{tip.X - d.X * half * 0.8f + n.X * half * s, tip.Y - d.Y * half * 0.8f + n.Y * half * s};
						auto strip = Outward(ThickLine({tip, wing}, 0.07f));
						auto v = To3D(strip);
						for (auto& q : v)
							q.Z = 0.03f;
						art.Quad(v[0], v[1], v[2], v[3], ArtSolid(chevron));
					}
				}
			}
			for (auto side : {&left, &right})
			{
				for (size_t i = 0; i + 1 < side->size(); i++)
					art.Prism(To3D(Outward(ThickLine({(*side)[i], (*side)[i + 1]}, 0.05f))), 0.3f, 0.42f,
					          ArtSolid(rail), ArtSolid(rail));
				for (auto& p : *side)
				{
					art.Cylinder(p.X, p.Y, 0.05f, 0.3f, 0.42f, ArtSolid(rail), ArtSolid(rail), 12);
					art.Cylinder(p.X, p.Y, 0.03f, 0, 0.3f, ArtSolid(rail), ArtSolid(rail), 8);
				}
			}
			// The exit flap.
			art.Prism(To3D(Outward(ThickLine({exitA, exitB}, 0.03f))), 0.25f, 0.42f, ArtSolid(rail), ArtSolid(rail));
		});
		return true;
	}

	bool Builder::AddGate(const Value& part, std::string& error)
	{
		// A one-way gate: a flap the ball can push through from one side only (the right-hand side when
		// looking from the first point to the second).
		auto pts = ReadPoints(part["points"]);
		if (pts.size() != 2)
		{
			error = "A gate needs 2 points";
			return false;
		}
		auto a = pts[0], b = pts[1];
		GroupBuild g;
		g.ShortValue(200);
		g.Shorts({300, MatWall});
		g.Floats({600.0f, 2.0f, a.X, a.Y, b.X, b.Y, -1.0f});
		auto group = Commit(Dat, g);
		Objects.push_back({1016, group});
		if (part["score"].Int() > 0)
			customtable::Scores[group] = part["score"].Int();

		auto metal = Metal(Color(part["color"], PostColor));
		Scenery.push_back([=](Art3D& art)
		{
			art.Prism(To3D(Outward(ThickLine({a, b}, 0.03f))), 0.25f, 0.5f, ArtSolid(metal), ArtSolid(metal));
			art.Cylinder(a.X, a.Y, 0.06f, 0, 0.55f, ArtSolid(metal), ArtSolid(metal), 10);
			art.Cylinder(b.X, b.Y, 0.06f, 0, 0.55f, ArtSolid(metal), ArtSolid(metal), 10);
		});
		return true;
	}

	bool Builder::ReadRules(std::string& error)
	{
		auto& rules = customtable::Rules;
		rules = CustomRules();
		auto& doc = Doc["rules"];
		auto lookup = [&](const Value& v, int& out, const char* what) -> bool
		{
			out = -1;
			auto id = v.Str();
			if (id.empty())
				return true;
			auto it = Ids.find(id);
			if (it == Ids.end())
			{
				error = std::string("no part with id \"") + id + "\" (" + what + ")";
				return false;
			}
			out = it->second;
			return true;
		};

		std::vector<std::string> missionNames;
		for (auto& g : doc["groups"].Items())
		{
			CustomRuleGroup group;
			group.Name = g["name"].Str("GROUP");
			for (auto& id : g["parts"].Items())
			{
				int index;
				if (!lookup(id, index, "group part"))
					return false;
				group.Parts.push_back(index);
			}
			for (auto& id : g["lights"].Items())
			{
				int index;
				if (!lookup(id, index, "group light"))
					return false;
				group.Lights.push_back(index);
			}
			// A group still being set up in the editor does nothing yet.
			if (group.Parts.empty())
				continue;
			group.Hits = g["hits"].Int(0);
			group.Score = g["score"].Int(5000);
			group.Bonus = g["bonus"].Int(0);
			group.Award = g["award"].Str("none");
			group.Message = g["message"].Str();
			group.IsMission = g["mission"].Bool(false);
			group.Time = std::max(0.0f, g["time"].Float(0));
			group.Hint = g["hint"].Str();
			if (group.Award != "none" && group.Award != "multiplier" && group.Award != "extraBall" &&
				group.Award != "multiball" && group.Award != "startMission")
			{
				error = "group " + group.Name + ": award must be none, multiplier, extraBall, multiball or startMission";
				return false;
			}
			rules.Groups.push_back(group);
			missionNames.push_back(g["startsMission"].Str());
		}
		// Missions are named by group name; link them once every group is known.
		for (size_t i = 0; i < rules.Groups.size(); i++)
		{
			if (rules.Groups[i].Award != "startMission")
				continue;
			for (size_t j = 0; j < rules.Groups.size(); j++)
				if (j != i && rules.Groups[j].IsMission && rules.Groups[j].Name == missionNames[i])
					rules.Groups[i].StartsMission = static_cast<int>(j);
		}
		rules.BallSave = std::max(0.0f, std::min(60.0f, doc["ballSave"].Float(0)));
		if (!lookup(doc["extraBallLight"], rules.ExtraBallLight, "extraBallLight"))
			return false;
		for (auto& id : doc["multiplierLights"].Items())
		{
			int index;
			if (!lookup(id, index, "multiplierLights"))
				return false;
			rules.MultiplierLights.push_back(index);
		}
		return true;
	}

	void Builder::PaintPlayfieldTexture()
	{
		TexPerUnit = std::min(64.0f, 20.0f * Scale);
		auto tw = static_cast<int>(W * TexPerUnit), th = static_cast<int>(L * TexPerUnit);
		PlayfieldTex.Width = tw;
		PlayfieldTex.Height = th;
		PlayfieldTex.RGB.assign(static_cast<size_t>(tw) * th * 3, 0);

		Art2D tex(tw, th, 1);
		auto u = [&](float x) { return (x + W / 2) * TexPerUnit; };
		auto v = [&](float y) { return (y + L / 2) * TexPerUnit; };

		// A picture from the table file covers the whole playfield; the light inserts are painted on top.
		bool picture = false;
		auto imageName = Doc["playfield"]["image"].Str();
		auto it = Files.find(imageName);
		if (!imageName.empty() && it != Files.end())
		{
			int iw, ih, channels;
			auto pixels = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(it->second.data()),
			                                    static_cast<int>(it->second.size()), &iw, &ih, &channels, 3);
			if (pixels)
			{
				tex.Picture(pixels, iw, ih);
				stbi_image_free(pixels);
				picture = true;
			}
		}

		if (!picture)
		{
			tex.VerticalGradient(0, 0, static_cast<float>(tw), static_cast<float>(th), Playfield, Playfield2);

			auto decor = Doc["playfield"]["decor"].Str("grid");
			if (decor == "space")
			{
				// Deep space: nebula clouds, a ringed planet and a field of stars (all from a fixed seed, so
				// the table looks the same every time).
				uint32_t seed = 20261102u;
				auto rnd = [&seed]()
				{
					seed = seed * 1664525u + 1013904223u;
					return (seed >> 8) / 16777216.0f;
				};
				tex.Glow(u(-3.5f), v(-4.0f), 7.0f * TexPerUnit, Accent, 0.22f);
				tex.Glow(u(4.0f), v(3.5f), 6.0f * TexPerUnit, Accent2, 0.18f);
				tex.Glow(u(-1.0f), v(9.0f), 5.0f * TexPerUnit, Accent.Mix(Accent2, 0.5f), 0.12f);
				for (int i = 0; i < static_cast<int>(W * L * 1.6f); i++)
				{
					auto sx = rnd() * W - W / 2, sy = rnd() * L - L / 2;
					auto big = rnd();
					auto r = (big > 0.97f ? 0.07f : big > 0.85f ? 0.045f : 0.025f) * TexPerUnit;
					auto tint = rnd();
					ArtColor c = tint < 0.15f ? Accent2.Mix({1, 1, 1}, 0.6f) : tint < 0.25f ? Accent.Mix({1, 1, 1}, 0.6f)
						: ArtColor{0.95f, 0.95f, 1.0f};
					tex.Circle(u(sx), v(sy), r, c, 0.35f + 0.65f * rnd());
					if (big > 0.97f)
						tex.Glow(u(sx), v(sy), r * 5, c, 0.25f);
				}
				// Orbit rings around the middle of the table.
				auto ring = [&](float cx, float cy, float rx, float ry, float tilt, ArtColor c, float alpha, float width)
				{
					const int n = 120;
					for (int i = 0; i < n; i++)
					{
						auto a0 = 2 * 3.14159265f * i / n, a1 = 2 * 3.14159265f * (i + 1) / n;
						auto px = [&](float a) { return cx + std::cos(a) * rx * std::cos(tilt) - std::sin(a) * ry * std::sin(tilt); };
						auto py = [&](float a) { return cy + std::cos(a) * rx * std::sin(tilt) + std::sin(a) * ry * std::cos(tilt); };
						tex.Line(u(px(a0)), v(py(a0)), u(px(a1)), v(py(a1)), width * TexPerUnit, c, alpha);
					}
				};
				auto pcx = Doc["playfield"]["planetX"].Float(0.0f), pcy = Doc["playfield"]["planetY"].Float(1.0f);
				ring(pcx, pcy, 5.2f, 1.8f, -0.35f, Accent2, 0.35f, 0.05f);
				ring(pcx, pcy, 6.4f, 2.3f, -0.35f, Accent, 0.22f, 0.03f);
				auto planet = Playfield.Mix(Accent2, 0.35f);
				tex.Glow(u(pcx), v(pcy), 2.6f * TexPerUnit, Accent2, 0.3f);
				tex.Circle(u(pcx), v(pcy), 1.5f * TexPerUnit, planet.Scaled(0.75f));
				tex.Circle(u(pcx - 0.25f), v(pcy - 0.3f), 1.15f * TexPerUnit, planet, 0.8f);
				tex.Circle(u(pcx - 0.5f), v(pcy - 0.6f), 0.55f * TexPerUnit, planet.Mix({1, 1, 1}, 0.25f), 0.5f);
			}
			else if (decor == "grid")
			{
				// Faint grid.
				for (float gx = -W / 2 + 1; gx < W / 2; gx += 1)
					tex.Line(u(gx), 0, u(gx), static_cast<float>(th), TexPerUnit * 0.03f, {1, 1, 1}, 0.05f);
				for (float gy = -L / 2 + 1; gy < L / 2; gy += 1)
					tex.Line(0, v(gy), static_cast<float>(tw), v(gy), TexPerUnit * 0.03f, {1, 1, 1}, 0.05f);
			}

			// Halos under bumpers, and rings around the bumper group.
			float cx = 0, cy = 0;
			int bumpers = 0;
			for (auto& part : Doc["parts"].Items())
			{
				if (part["type"].Str() != "bumper")
					continue;
				auto bx = part["x"].Float(), by = part["y"].Float(), br = part["radius"].Float(0.55f);
				tex.Glow(u(bx), v(by), br * 2.4f * TexPerUnit, Color(part["capColor"], Accent2), 0.35f);
				cx += bx;
				cy += by;
				bumpers++;
			}
			if (bumpers > 1)
			{
				cx /= bumpers;
				cy /= bumpers;
				tex.StrokeRect(u(cx) - 3.2f * TexPerUnit, v(cy) - 3.2f * TexPerUnit, 6.4f * TexPerUnit,
				               6.4f * TexPerUnit, TexPerUnit * 0.08f, Accent2, 3.2f * TexPerUnit, 0.45f);
				tex.StrokeRect(u(cx) - 3.6f * TexPerUnit, v(cy) - 3.6f * TexPerUnit, 7.2f * TexPerUnit,
				               7.2f * TexPerUnit, TexPerUnit * 0.03f, Accent2, 3.6f * TexPerUnit, 0.35f);
			}
		}

		// Kickout holes and drop target slots are cut into the playfield.
		for (auto& part : Doc["parts"].Items())
		{
			auto type = part["type"].Str();
			auto px = part["x"].Float(), py = part["y"].Float();
			if (type == "kickout")
			{
				auto r = part["radius"].Float(0.45f);
				tex.Circle(u(px), v(py), r * 1.12f * TexPerUnit, Color(part["color"], Accent), 0.8f);
				tex.Circle(u(px), v(py), r * TexPerUnit, {0.02f, 0.02f, 0.03f});
				tex.Glow(u(px), v(py), r * 0.8f * TexPerUnit, {0, 0, 0}, 0.6f);
			}
			else if (type == "drop")
			{
				auto width = part["width"].Float(0.8f);
				auto a = part["angle"].Float(90.0f) * 3.14159265f / 180.0f;
				P2 along{-std::sin(a), std::cos(a)};
				tex.Line(u(px - along.X * width / 2), v(py - along.Y * width / 2), u(px + along.X * width / 2),
				         v(py + along.Y * width / 2), 0.26f * TexPerUnit, {0.03f, 0.03f, 0.04f});
			}
		}

		// Light inserts, unlit, with their labels.
		for (auto& part : Doc["parts"].Items())
		{
			if (part["type"].Str() != "light")
				continue;
			auto lx = part["x"].Float(), ly = part["y"].Float(), lr = part["radius"].Float(0.35f);
			auto lc = Color(part["color"], Accent);
			auto unlit = lc.Scaled(0.5f).Mix(Playfield2, 0.4f);
			if (part["shape"].Str() == "arrow")
			{
				auto a = (part["angle"].Float(-90.0f)) * 3.14159265f / 180.0f;
				auto dx = std::cos(a), dy = std::sin(a);
				auto tipX = lx + dx * lr * 1.3f, tipY = ly + dy * lr * 1.3f;
				for (int side = -1; side <= 1; side += 2)
				{
					auto bx = lx - dx * lr * 0.6f - dy * lr * side, by = ly - dy * lr * 0.6f + dx * lr * side;
					tex.Line(u(tipX), v(tipY), u(bx), v(by), lr * 0.55f * TexPerUnit, unlit);
				}
			}
			else
			{
				tex.Circle(u(lx), v(ly), lr * 1.18f * TexPerUnit, Playfield2.Scaled(0.6f));
				tex.Circle(u(lx), v(ly), lr * TexPerUnit, unlit);
			}
			auto label = part["label"].Str();
			if (!label.empty())
			{
				auto ls = TexPerUnit * part["labelSize"].Float(0.32f);
				tex.Text(label, u(lx), v(ly + lr * 1.35f), ls, ls * 0.16f, {0.92f, 0.92f, 0.95f}, 1, 0.85f);
			}
		}

		// The table's name, painted across the lower playfield (own pictures carry their own).
		auto name = picture ? std::string() : Doc["name"].Str("My Table");
		auto size = TexPerUnit * 0.9f;
		auto maxWidth = W * 0.7f * TexPerUnit;
		auto textWidth = Art2D::TextWidth(name, size);
		if (textWidth > maxWidth)
			size *= maxWidth / textWidth;
		auto nameY = v(Doc["playfield"]["nameY"].Float(L * 0.16f));
		tex.Text(name, u(Doc["playfield"]["nameX"].Float(0)), nameY, size, size * 0.14f, Accent, 1, 0.85f);

		// Darker area around the drain.
		if (!picture)
			tex.VerticalGradient(0, v(L / 2 - 2.5f), static_cast<float>(tw), 2.5f * TexPerUnit, Playfield2,
			                     Playfield2.Scaled(0.5f));

		auto img = tex.Resolve(1);
		for (size_t i = 0; i < img.Pixels.size(); i++)
		{
			ColorRgba c{img.Pixels[i]};
			PlayfieldTex.RGB[i * 3] = c.GetRed();
			PlayfieldTex.RGB[i * 3 + 1] = c.GetGreen();
			PlayfieldTex.RGB[i * 3 + 2] = c.GetBlue();
		}
	}

	int Builder::BuildTableLayer()
	{
		Art3D art(Camera, 0, 0, TableW, ScreenH, Scale);
		auto x0 = -W / 2, x1 = W / 2, y0 = -L / 2, y1 = L / 2;

		// Playfield.
		auto tex = &PlayfieldTex;
		auto texPerUnit = TexPerUnit;
		auto halfW = W / 2, halfL = L / 2;
		Art3D::Shader playfield = [tex, texPerUnit, halfW, halfL](const ArtVec& pos, const ArtVec& n)
		{
			ArtMaterial m;
			m.Color = tex->Sample((pos.X + halfW) * texPerUnit, (pos.Y + halfL) * texPerUnit);
			m.Ambient = 0.55f;
			m.Diffuse = 0.5f;
			m.Specular = 0.12f;
			m.Shininess = 12;
			return ArtShade(m, pos, n);
		};
		art.Quad({x0, y0, 0}, {x1, y0, 0}, {x1, y1, 0}, {x0, y1, 0}, playfield);

		// Cabinet: side rails, back rail and the apron at the front.
		auto wood = RubberMat(Cabinet);
		auto woodTop = RubberMat(Cabinet.Mix({1, 1, 1}, 0.08f));
		auto railTop = Metal(Cabinet.Mix(PostColor, 0.35f));
		art.Prism({{x0 - Rail, y0 - Rail, 0}, {x0, y0 - Rail, 0}, {x0, y1 + Apron, 0}, {x0 - Rail, y1 + Apron, 0}},
		          0, RailH, ArtSolid(railTop), ArtSolid(wood));
		art.Prism({{x1, y0 - Rail, 0}, {x1 + Rail, y0 - Rail, 0}, {x1 + Rail, y1 + Apron, 0}, {x1, y1 + Apron, 0}},
		          0, RailH, ArtSolid(railTop), ArtSolid(wood));
		art.Prism({{x0, y0 - Rail, 0}, {x1, y0 - Rail, 0}, {x1, y0, 0}, {x0, y0, 0}},
		          0, RailH, ArtSolid(railTop), ArtSolid(wood));
		art.Prism({{x0, y1, 0}, {x1, y1, 0}, {x1, y1 + Apron, 0}, {x0, y1 + Apron, 0}},
		          0, 0.35f, ArtSolid(woodTop), ArtSolid(wood));

		for (auto& paint : Scenery)
			paint(art);

		auto one = art.Resolve(art.SuperScale());
		ArtImage hd;
		if (Scale > 1)
			hd = art.Resolve(art.SS);
		gdrv_bitmap8* bmp;
		zmap_header_type* zMap;
		MakeSprite(one, hd, 0, 0, true, bmp, zMap);

		GroupBuild g;
		g.ShortValue(200);
		g.Name("table");
		g.Sprite(bmp, zMap);
		g.Shorts({100, 1, 300, MatWall, 400, Kicker(5.0f, 4.0f)});
		// Safety boundary along the cabinet edges; corner order keeps every edge facing in.
		auto bx0 = x0 - 0.05f, bx1 = x1 + 0.05f, by0 = y0 - 0.05f, by1 = y1 + Apron;
		g.Floats({600.0f, 5.0f, bx1, by1, bx0, by1, bx0, by0, bx1, by0, bx1, by1, -1.0f});
		auto slope = Doc["physics"]["slope"].Float(0.5f);
		g.Floats({305.0f, 25.0f, slope, 1.570796f});
		g.Floats({701.0f, Doc["physics"]["friction"].Float(0.2f)});
		g.Floats({700.0f, Camera.CenterX, Camera.CenterY});
		return Commit(Dat, g);
	}

	int Builder::BuildBall()
	{
		const int frames = 7;
		const float radius = 0.3f;
		StatefulPart sp;
		sp.Main.ShortValue(200);
		sp.Main.Name("ball");
		sp.Main.Shorts({100, frames});
		sp.Main.Floats({500.0f, radius});

		std::vector<float> ys;
		for (int i = 0; i < frames; i++)
			ys.push_back(-L / 2 + L * (i + 0.5f) / frames);

		auto ballColor = Color(Doc["theme"]["ball"], {0.82f, 0.84f, 0.88f});
		for (int i = 0; i < frames; i++)
		{
			float sx, sy, depth, sx2, sy2;
			Camera.Project({0, ys[i], radius}, sx, sy, depth);
			Camera.Project({radius, ys[i], radius}, sx2, sy2, depth);
			auto pixelRadius = std::fabs(sx2 - sx);
			auto size = static_cast<int>(std::ceil(pixelRadius * 2)) + 2;
			if (size % 2 == 0)
				size++;

			auto paint = [&](int scale)
			{
				ArtImage img;
				img.Width = img.Height = size * scale;
				img.Pixels.assign(static_cast<size_t>(img.Width) * img.Height, 0);
				img.Depth.assign(img.Pixels.size(), 0);
				auto rPix = pixelRadius * scale;
				auto c = img.Width * 0.5f;
				const int ss = 4;
				for (int py = 0; py < img.Height; py++)
					for (int px = 0; px < img.Width; px++)
					{
						float r = 0, g = 0, b = 0;
						int hits = 0;
						for (int s = 0; s < ss * ss; s++)
						{
							auto fx = (px + (s % ss + 0.5f) / ss - c) / rPix;
							auto fy = (py + (s / ss + 0.5f) / ss - c) / rPix;
							auto d2 = fx * fx + fy * fy;
							if (d2 > 1)
								continue;
							hits++;
							ArtVec n{fx, fy, std::sqrt(1 - d2)};
							// Chrome: reflects a bright sky above and a dark playfield below, plus the light.
							auto sky = Clamp01(0.5f - n.Y * 0.6f);
							auto env = ballColor.Scaled(0.35f + 0.65f * sky);
							auto l = ArtNormalize({-0.45f, -0.55f, 0.7f});
							auto h = ArtNormalize(l + ArtVec{0, 0, 1});
							auto spec = std::pow(std::max(0.0f, ArtDot(n, h)), 40.0f) * 1.1f;
							auto rim = 0.55f + 0.45f * n.Z;
							r += env.R * rim + spec;
							g += env.G * rim + spec;
							b += env.B * rim + spec;
						}
						if (hits * 2 < ss * ss)
							continue;
						r /= hits;
						g /= hits;
						b /= hits;
						img.Pixels[static_cast<size_t>(py) * img.Width + px] =
							ColorRgba{
								static_cast<uint8_t>(Clamp01(r) * 255), static_cast<uint8_t>(Clamp01(g) * 255),
								static_cast<uint8_t>(Clamp01(b) * 255), 255
							}.Color;
					}
				return img;
			};

			auto one = paint(1);
			ArtImage hd;
			if (Scale > 1)
				hd = paint(Scale);
			gdrv_bitmap8* bmp;
			zmap_header_type* zMap;
			MakeSprite(one, hd, 0, 0, false, bmp, zMap);
			if (i == 0)
			{
				sp.Main.Sprite(bmp, nullptr);
				sp.Main.Floats({501.0f, 0, ys[i], radius});
			}
			else
				sp.States.push_back({bmp, nullptr});
		}

		// State records also carry the depth that picks each image.
		auto first = Commit(Dat, sp.Main);
		for (int i = 1; i < frames; i++)
		{
			GroupBuild g;
			g.ShortValue(201);
			g.Sprite(sp.States[i - 1].first, nullptr);
			g.Floats({501.0f, 0, ys[i], radius});
			Commit(Dat, g);
		}
		return first;
	}

	int Builder::BuildDigits()
	{
		const int w = 15, h = 26;
		auto color = Color(Doc["theme"]["display"], {1.0f, 0.62f, 0.18f});
		int first = -1;
		for (int d = 0; d < 10; d++)
		{
			auto paint = [&](int scale)
			{
				Art2D art(w, h, scale);
				art.Clear({0, 0, 0});
				std::string text(1, static_cast<char>('0' + d));
				art.Text(text, w * 0.5f, 4, 18, 2.4f, color, 1);
				return art.Resolve(1);
			};
			auto one = paint(1);
			ArtImage hd;
			if (Scale > 1)
				hd = paint(Scale);
			gdrv_bitmap8* bmp;
			zmap_header_type* zMap;
			MakeSprite(one, hd, 0, 0, false, bmp, zMap);
			GroupBuild g;
			if (d == 0)
				g.Name("score_digits");
			g.Sprite(bmp, nullptr);
			auto index = Commit(Dat, g);
			if (d == 0)
				first = index;
		}
		return first;
	}

	void Builder::BuildPanel(int& scoreGroup, int& ballGroup, int& playerGroup, int& infoGroup, int& missionGroup)
	{
		auto pw = ScreenW - TableW;
		auto digits = BuildDigits();
		auto display = Color(Doc["theme"]["display"], {1.0f, 0.62f, 0.18f});

		// Screen areas (absolute 1x coordinates).
		const int scoreY = 190, scoreH = 26, rowY = 236, infoY = 280, infoH = 40, missionY = 334, missionH = 70;
		auto left = TableW + 14, inner = pw - 28;

		auto paint = [&](int scale)
		{
			Art2D art(pw, ScreenH, scale);
			art.VerticalGradient(0, 0, static_cast<float>(pw), static_cast<float>(ScreenH), PanelColor.Scaled(1.25f),
			                     PanelColor.Scaled(0.6f));
			art.StrokeRect(4, 4, pw - 8.0f, ScreenH - 8.0f, 2, PostColor.Scaled(0.6f), 8);

			// Title plate.
			art.VerticalGradient(10, 10, pw - 20.0f, 150, Playfield.Mix({0, 0, 0}, 0.2f), Playfield2, 8);
			art.StrokeRect(10, 10, pw - 20.0f, 150, 1.5f, Accent, 8, 0.9f);
			art.Text("PINBALL WIZZ", pw * 0.5f, 24, 8, 1.2f, Accent2, 1, 0.9f);
			auto name = Doc["name"].Str("My Table");
			// Wrap the name onto two lines when it is long.
			std::vector<std::string> lines{name};
			auto maxW = pw - 40.0f;
			if (Art2D::TextWidth(name, 22) > maxW)
			{
				auto mid = name.size() / 2;
				auto split = name.rfind(' ', mid);
				auto after = name.find(' ', mid);
				if (split == std::string::npos || (after != std::string::npos && after - mid < mid - split))
					split = after;
				if (split != std::string::npos)
					lines = {name.substr(0, split), name.substr(split + 1)};
			}
			float size = 22;
			for (auto& line : lines)
				size = std::min(size, maxW / std::max(1.0f, Art2D::TextWidth(line, 1)));
			auto y = lines.size() == 1 ? 85 - size * 0.5f : 68 - size * 0.5f;
			for (auto& line : lines)
			{
				art.Glow(pw * 0.5f, y + size * 0.5f, Art2D::TextWidth(line, size) * 0.6f, Accent, 0.12f);
				art.Text(line, pw * 0.5f, y, size, size * 0.15f, {1, 1, 1}, 1);
				y += size * 1.6f;
			}
			auto author = Doc["author"].Str();
			if (!author.empty())
				art.Text("BY " + author, pw * 0.5f, 140, 7, 1.0f, {0.75f, 0.78f, 0.85f}, 1);

			// Displays.
			auto box = [&](float x, float y, float w, float h)
			{
				art.FillRect(x - 4, y - 4, w + 8, h + 8, {0.01f, 0.01f, 0.02f}, 4);
				art.StrokeRect(x - 4, y - 4, w + 8, h + 8, 1.5f, PostColor.Scaled(0.7f), 4);
			};
			box(14, scoreY, inner, scoreH);
			art.Text("PLAYER", 14, rowY + 8, 9, 1.4f, display, 0, 0.9f);
			box(80, rowY, 16, scoreH);
			art.Text("BALL", pw - 52.0f - 14, rowY + 8, 9, 1.4f, display, 2, 0.9f);
			box(pw - 30.0f - 14, rowY, 16, scoreH);
			box(14, infoY, inner, infoH);
			box(14, missionY, inner, missionH);
			return art.Resolve(1);
		};

		auto one = paint(1);
		ArtImage hd;
		if (Scale > 1)
			hd = paint(Scale);
		gdrv_bitmap8* bmp;
		zmap_header_type* zMap;
		MakeSprite(one, hd, TableW, 0, false, bmp, zMap);
		GroupBuild bg;
		bg.Name("background");
		bg.Sprite(bmp, nullptr);
		Commit(Dat, bg);

		auto scoreBox = [&](const char* name, int x, int y, int w, int h)
		{
			GroupBuild g;
			g.Name(name);
			g.Shorts({digits, x, y, w, h});
			return Commit(Dat, g);
		};
		scoreGroup = scoreBox("score1", left, scoreY, inner, scoreH);
		playerGroup = scoreBox("player_number1", TableW + 80, rowY, 16, scoreH);
		ballGroup = scoreBox("ballcount1", TableW + pw - 30 - 14, rowY, 16, scoreH);

		auto textBox = [&](const char* name, int y, int h)
		{
			GroupBuild g;
			g.ShortValue(200);
			g.Name(name);
			g.Shorts({1500, left, y, inner, h});
			return Commit(Dat, g);
		};
		infoGroup = textBox("info_text_box", infoY, infoH);
		missionGroup = textBox("mission_text_box", missionY, missionH);
	}

	DatFile* Builder::Build(std::string& error)
	{
		Scale = customtable::BuildScale;
		W = Doc["playfield"]["width"].Float(16.0f);
		L = Doc["playfield"]["length"].Float(29.0f);
		if (W < 6 || W > 30 || L < 12 || L > 50)
		{
			error = "The playfield size is out of range (width 6 to 30, length 12 to 50)";
			return nullptr;
		}

		auto& theme = Doc["theme"];
		Playfield = Color(Doc["playfield"]["color"], {0.09f, 0.15f, 0.3f});
		Playfield2 = Color(Doc["playfield"]["color2"], Playfield.Scaled(0.45f));
		WallColor = Color(theme["walls"], {0.72f, 0.77f, 0.86f});
		Rubber = Color(theme["rubber"], {0.95f, 0.95f, 0.93f});
		Accent = Color(theme["accent"], {1.0f, 0.55f, 0.12f});
		Accent2 = Color(theme["accent2"], {0.2f, 0.8f, 1.0f});
		Cabinet = Color(theme["cabinet"], {0.12f, 0.1f, 0.14f});
		PanelColor = Color(theme["panel"], {0.06f, 0.07f, 0.12f});
		PostColor = Color(theme["metal"], {0.78f, 0.8f, 0.86f});

		Dat = new DatFile();
		Dat->AppName = "Pinball Wizz";
		Dat->Description = Doc["name"].Str();

		GroupBuild header;
		header.Name("pinball_wizz_custom_table");
		Commit(Dat, header);

		SetupCamera();
		{
			GroupBuild g;
			g.Name("camera_info");
			std::vector<float> cam(Camera.Matrix, Camera.Matrix + 12);
			cam.push_back(Camera.D);
			cam.push_back(Camera.ZMin);
			cam.push_back(Camera.ZScaler);
			cam.push_back(Camera.CenterX);
			cam.push_back(Camera.CenterY);
			g.Floats(cam);
			Commit(Dat, g);
		}

		MatWall = Material(0.85f, 0.5f);
		MatRubber = Material(0.9f, 0.75f);
		MatFlipper = Material(0.5f, 0.75f);
		MatMetal = Material(0.95f, 0.4f);

		PaintPlayfieldTexture();

		bool haveLeft = false, haveRight = false, havePlunger = false, haveDrain = false;
		for (auto& part : Doc["parts"].Items())
		{
			auto type = part["type"].Str();
			auto objectsBefore = Objects.size();
			bool ok = true;
			if (type == "wall")
				ok = AddWall(part, error);
			else if (type == "post")
				ok = AddPost(part, error);
			else if (type == "slingshot")
				ok = AddSlingshot(part, error);
			else if (type == "bumper")
				ok = AddBumper(part, error);
			else if (type == "flipper")
			{
				auto left = part["side"].Str("left") != "right";
				if ((left && haveLeft) || (!left && haveRight))
				{
					error = "This version supports one left and one right flipper";
					ok = false;
				}
				else
				{
					ok = AddFlipper(part, error);
					(left ? haveLeft : haveRight) = true;
				}
			}
			else if (type == "plunger")
			{
				if (havePlunger)
				{
					error = "A table can have only one plunger";
					ok = false;
				}
				else
				{
					ok = AddPlunger(part, error);
					havePlunger = true;
				}
			}
			else if (type == "drain")
			{
				if (!haveDrain)
					ok = AddDrain(part, error);
				haveDrain = true;
			}
			else if (type == "lane")
				ok = AddLane(part, error);
			else if (type == "target")
				ok = AddTarget(part, error);
			else if (type == "light")
				ok = AddLight(part, error);
			else if (type == "drop")
				ok = AddDropTarget(part, error);
			else if (type == "kickout")
				ok = AddKickout(part, error);
			else if (type == "spinner")
				ok = AddSpinner(part, error);
			else if (type == "ramp")
				ok = AddRamp(part, error);
			else if (type == "gate")
				ok = AddGate(part, error);
			else
				printf("Custom table: skipping unknown part type \"%s\"\n", type.c_str());

			auto id = part["id"].Str();
			if (ok && !id.empty() && Objects.size() > objectsBefore)
				Ids[id] = Objects.back().second;

			if (!ok)
			{
				auto id = part["id"].Str();
				error = "Part " + (id.empty() ? type : id) + ": " + error;
				delete Dat;
				Dat = nullptr;
				return nullptr;
			}
		}

		if (!haveDrain)
		{
			Value drain = Value::MakeObject();
			AddDrain(drain, error);
		}
		if (!ReadRules(error))
		{
			error = "Rules: " + error;
			delete Dat;
			Dat = nullptr;
			return nullptr;
		}
		if (!haveLeft || !haveRight || !havePlunger)
		{
			error = "A table needs a left flipper, a right flipper and a plunger";
			delete Dat;
			Dat = nullptr;
			return nullptr;
		}

		BuildTableLayer();
		BuildBall();
		int scoreGroup, ballGroup, playerGroup, infoGroup, missionGroup;
		BuildPanel(scoreGroup, ballGroup, playerGroup, infoGroup, missionGroup);
		Objects.push_back({1033, infoGroup});
		Objects.push_back({1033, missionGroup});

		GroupBuild objects;
		objects.Name("table_objects");
		std::vector<int> list{1025};
		for (auto& obj : Objects)
		{
			list.push_back(obj.first);
			list.push_back(obj.second);
		}
		objects.Shorts(list);
		Commit(Dat, objects);

		Dat->Finalize();
		return Dat;
	}

	int ChooseScale()
	{
		auto detail = options::Options.HdDetail.V;
		if (detail == 1)
			return 1;
		if (detail <= 0)
			return hdgfx::AutoScale(4, 416);
		return std::min(detail, 4);
	}
}

namespace
{
	uint32_t PackColor(const ArtColor& c)
	{
		auto ch = [](float v) { return static_cast<uint32_t>(Clamp01(v) * 255.0f + 0.5f); };
		return ch(c.R) | ch(c.G) << 8 | ch(c.B) << 16 | 0xFFu << 24;
	}

}

void customtable::BuildPreview(const Value& doc, TablePreview& out)
{
		// Same default colours as the table builder.
		auto& theme = doc["theme"];
		auto color = [](const Value& v, ArtColor def) { return ArtColor::FromHex(v.Str(), def); };
		auto playfield = color(doc["playfield"]["color"], {0.09f, 0.15f, 0.3f});
		auto walls = color(theme["walls"], {0.72f, 0.77f, 0.86f});
		auto rubber = color(theme["rubber"], {0.95f, 0.95f, 0.93f});
		auto accent = color(theme["accent"], {1.0f, 0.55f, 0.12f});
		auto accent2 = color(theme["accent2"], {0.2f, 0.8f, 1.0f});
		out.Width = doc["playfield"]["width"].Float(16.0f);
		out.Length = doc["playfield"]["length"].Float(29.0f);
		out.Playfield = PackColor(playfield);
		out.Playfield2 = PackColor(color(doc["playfield"]["color2"], playfield.Scaled(0.45f)));
		out.Accent = PackColor(accent);
		out.Shapes.clear();

		auto points = [](const Value& v)
		{
			std::vector<float> pts;
			for (auto& p : v.Items())
			{
				pts.push_back(p[static_cast<size_t>(0)].Float());
				pts.push_back(p[static_cast<size_t>(1)].Float());
			}
			return pts;
		};
		int partIndex = -1;
		for (auto& part : doc["parts"].Items())
		{
			partIndex++;
			auto type = part["type"].Str();
			TablePreviewShape shape;
			shape.PartIndex = partIndex;
			if (type == "wall")
			{
				shape.Kind = TablePreviewShape::Polygon;
				shape.Points = points(part["points"]);
				shape.Filled = part["closed"].Bool(false);
				shape.Width = part["thickness"].Float(0.3f);
				shape.Color = PackColor(color(part["color"], walls));
			}
			else if (type == "slingshot")
			{
				shape.Kind = TablePreviewShape::Polygon;
				shape.Points = points(part["points"]);
				shape.Filled = true;
				shape.Color = PackColor(color(part["color"], accent));
			}
			else if (type == "post" || type == "bumper")
			{
				auto bumper = type == "bumper";
				shape.Kind = TablePreviewShape::Circle;
				shape.Points = {part["x"].Float(), part["y"].Float()};
				shape.Radius = part["radius"].Float(bumper ? 0.55f : 0.2f);
				shape.Filled = true;
				shape.Color = PackColor(bumper ? color(part["capColor"], accent2) : rubber);
			}
			else if (type == "flipper")
			{
				auto left = part["side"].Str("left") != "right";
				auto x = part["x"].Float(), y = part["y"].Float(), length = part["length"].Float(2.0f);
				auto a = part["angle"].Float(30.0f) * 3.14159265f / 180.0f;
				shape.Kind = TablePreviewShape::Line;
				shape.Points = {x, y, x + (left ? 1 : -1) * std::cos(a) * length, y + std::sin(a) * length};
				shape.Width = 0.45f;
				shape.Color = PackColor(color(part["color"], {0.95f, 0.95f, 0.97f}));
			}
			else if (type == "plunger")
			{
				auto x = part["x"].Float(), y = part["y"].Float();
				shape.Kind = TablePreviewShape::Line;
				shape.Points = {x, y, x, y + 1.0f};
				shape.Width = part["width"].Float(0.7f) * 0.6f;
				shape.Color = PackColor(color(part["color"], accent));
			}
			else if (type == "lane")
			{
				shape.Kind = TablePreviewShape::Line;
				shape.Points = points(part["points"]);
				shape.Width = 0.08f;
				shape.Color = PackColor({0.75f, 0.78f, 0.85f});
			}
			else if (type == "target")
			{
				auto x = part["x"].Float(), y = part["y"].Float(), w = part["width"].Float(0.8f) / 2;
				auto a = part["angle"].Float(90.0f) * 3.14159265f / 180.0f;
				float fx = std::cos(a), fy = std::sin(a), ax = -fy, ay = fx, d = 0.08f;
				shape.Kind = TablePreviewShape::Polygon;
				shape.Points = {x - ax * w - fx * d, y - ay * w - fy * d, x + ax * w - fx * d, y + ay * w - fy * d,
				                x + ax * w + fx * d, y + ay * w + fy * d, x - ax * w + fx * d, y - ay * w + fy * d};
				shape.Filled = true;
				shape.Color = PackColor(color(part["color"], accent));
			}
			else if (type == "light")
			{
				shape.Kind = TablePreviewShape::Circle;
				shape.Points = {part["x"].Float(), part["y"].Float()};
				shape.Radius = part["radius"].Float(0.35f);
				shape.Filled = true;
				shape.Color = PackColor(color(part["color"], accent).Mix(playfield, 0.4f));
			}
			else if (type == "drop")
			{
				auto x = part["x"].Float(), y = part["y"].Float(), w = part["width"].Float(0.8f) / 2;
				auto a = part["angle"].Float(90.0f) * 3.14159265f / 180.0f;
				float fx = std::cos(a), fy = std::sin(a), ax = -fy, ay = fx, d = 0.12f;
				shape.Kind = TablePreviewShape::Polygon;
				shape.Points = {x - ax * w - fx * d, y - ay * w - fy * d, x + ax * w - fx * d, y + ay * w - fy * d,
				                x + ax * w + fx * d, y + ay * w + fy * d, x - ax * w + fx * d, y - ay * w + fy * d};
				shape.Filled = true;
				shape.Color = PackColor(color(part["color"], accent).Mix({1, 1, 1}, 0.2f));
			}
			else if (type == "kickout")
			{
				shape.Kind = TablePreviewShape::Circle;
				shape.Points = {part["x"].Float(), part["y"].Float()};
				shape.Radius = part["radius"].Float(0.45f);
				shape.Filled = true;
				shape.Color = PackColor({0.05f, 0.05f, 0.07f});
				// A rim, so the dark hole shows on dark playfields.
				TablePreviewShape rim = shape;
				rim.Radius *= 1.2f;
				rim.Color = PackColor(color(part["color"], accent));
				out.Shapes.push_back(rim);
			}
			else if (type == "spinner")
			{
				auto x = part["x"].Float(), y = part["y"].Float(), w = part["width"].Float(1.0f) / 2;
				auto a = part["angle"].Float(0.0f) * 3.14159265f / 180.0f;
				shape.Kind = TablePreviewShape::Line;
				shape.Points = {x - std::cos(a) * w, y - std::sin(a) * w, x + std::cos(a) * w, y + std::sin(a) * w};
				shape.Width = 0.14f;
				shape.Color = PackColor(color(part["color"], accent2));
			}
			else if (type == "gate")
			{
				shape.Kind = TablePreviewShape::Line;
				shape.Points = points(part["points"]);
				shape.Width = 0.08f;
				shape.Color = PackColor({0.85f, 0.85f, 0.6f});
				if (shape.Points.size() == 4)
				{
					// An arrow showing the way balls pass: from the right-hand side to the left-hand side.
					auto& q = shape.Points;
					float mx = (q[0] + q[2]) / 2, my = (q[1] + q[3]) / 2, dx = q[2] - q[0], dy = q[3] - q[1];
					auto len = std::sqrt(dx * dx + dy * dy);
					if (len > 0.01f)
					{
						float tx = dy / len, ty = -dx / len; // direction of travel
						TablePreviewShape arrow = shape;
						arrow.Width = 0.06f;
						arrow.Points = {mx - tx * 0.5f, my - ty * 0.5f, mx + tx * 0.3f, my + ty * 0.3f,
						                mx + tx * 0.05f - ty * 0.2f, my + ty * 0.05f + tx * 0.2f, mx + tx * 0.3f,
						                my + ty * 0.3f, mx + tx * 0.05f + ty * 0.2f, my + ty * 0.05f - tx * 0.2f};
						out.Shapes.push_back(arrow);
					}
				}
			}
			else if (type == "ramp")
			{
				shape.Kind = TablePreviewShape::Polygon;
				shape.Points = points(part["points"]);
				shape.Width = part["width"].Float(1.1f);
				shape.Color = (PackColor(color(part["color"], accent2)) & 0x00FFFFFFu) | (0x70u << 24);
			}
			else if (type == "drain")
			{
				auto y = part["y"].Float(out.Length / 2 - 0.3f);
				shape.Kind = TablePreviewShape::Line;
				shape.Points = {part["x0"].Float(-out.Width / 2), y, part["x1"].Float(out.Width / 2), y};
				shape.Width = 0.06f;
				shape.Color = PackColor({0.9f, 0.25f, 0.25f});
			}
			else
				continue;
			if (!shape.Points.empty())
				out.Shapes.push_back(shape);
		}
}

DatFile* customtable::Load(const std::string& path, std::string& error)
{
	Unload();
	Value doc;
	std::map<std::string, std::string> files;
	if (!ReadTableDocument(path, doc, files, error))
		return nullptr;

	// A plain table file can name a picture that sits next to it in the same folder.
	auto picture = doc["playfield"]["image"].Str();
	if (!picture.empty() && !files.count(picture) && picture.find_first_of("/\\") == std::string::npos &&
		picture.find("..") == std::string::npos)
	{
		auto cut = path.find_last_of("/\\");
		auto folder = cut == std::string::npos ? std::string() : path.substr(0, cut + 1);
		std::string bytes;
		if (ReadFileBytes(folder + picture, bytes))
			files[picture] = bytes;
	}

	BuildScale = ChooseScale();
	auto start = std::chrono::steady_clock::now();
	Builder builder(doc, files);
	auto dat = builder.Build(error);
	if (!dat)
	{
		Unload();
		return nullptr;
	}
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	printf("Custom table \"%s\" built in %d ms (detail %dx)\n", doc["name"].Str().c_str(), static_cast<int>(ms),
	       BuildScale);

	Active = true;
	Name = doc["name"].Str("Custom table");
	Path = path;
	BallsPerGame = std::max(1, std::min(9, doc["ballsPerGame"].Int(3)));
	return dat;
}

void customtable::ApplyColors()
{
	for (auto& art : Pending)
	{
		auto bmp = art.Bmp;
		if (!bmp || !bmp->BmpBufPtr1 || art.One.Width != bmp->Width || art.One.Height != bmp->Height)
			continue;
		for (int y = 0; y < bmp->Height; y++)
			std::memcpy(&bmp->BmpBufPtr1[bmp->Stride * y], &art.One.Pixels[static_cast<size_t>(y) * art.One.Width],
			            sizeof(uint32_t) * bmp->Width);
	}
}

void customtable::ApplyHd()
{
	if (!hdgfx::Active() || hdgfx::Scale != BuildScale)
		return;
	auto s = hdgfx::Scale;
	for (auto& art : Pending)
	{
		if (art.Hd.Width != art.One.Width * s || art.Hd.Height != art.One.Height * s)
			continue;
		auto twin = new gdrv_bitmap8(art.Hd.Width, art.Hd.Height, false);
		twin->XPosition = art.Bmp->XPosition * s;
		twin->YPosition = art.Bmp->YPosition * s;
		std::memcpy(twin->BmpBufPtr1, art.Hd.Pixels.data(), art.Hd.Pixels.size() * sizeof(uint32_t));
		hdgfx::SetTwin(art.Bmp, twin);
		if (art.ZMap)
		{
			auto zTwin = new zmap_header_type(art.Hd.Width, art.Hd.Height, art.Hd.Width);
			std::memcpy(zTwin->ZPtr1, art.Hd.Depth.data(), art.Hd.Depth.size() * sizeof(uint16_t));
			hdgfx::SetTwin(art.ZMap, zTwin);
		}
	}
	// HD images are not needed after this; free the memory. 1x images stay for ApplyColors.
	for (auto& art : Pending)
		art.Hd = ArtImage();
}

void customtable::Unload()
{
	Active = false;
	Name.clear();
	Path.clear();
	Scores.clear();
	Bonus.clear();
	Rules = CustomRules();
	Pending.clear();
	BallsPerGame = 3;
}

int customtable::ScoreFor(const TPinballComponent* component)
{
	if (!component)
		return 0;
	auto it = Scores.find(component->GroupIndex);
	return it == Scores.end() ? 0 : it->second;
}

bool customtable::ReadDocument(const std::string& path, Value& doc, bool& packed, std::string& error)
{
	std::map<std::string, std::string> files;
	if (!ReadTableDocument(path, doc, files, error))
		return false;
	std::string data;
	packed = ReadFileBytes(path, data) && data.size() >= 2 && data[0] == 'P' && data[1] == 'K';
	return true;
}

bool customtable::ReadInfo(const std::string& path, CustomTableEntry& entry)
{
	Value doc;
	std::map<std::string, std::string> files;
	std::string error;
	if (!ReadTableDocument(path, doc, files, error))
		return false;
	entry.Path = path;
	entry.Name = doc["name"].Str("Untitled table");
	entry.Author = doc["author"].Str();
	BuildPreview(doc, entry.Preview);
	return true;
}

std::vector<CustomTableEntry> customtable::FindTables(const std::vector<std::string>& baseFolders)
{
	std::vector<CustomTableEntry> result;
	std::vector<std::string> seen;
	for (auto& base : baseFolders)
	{
		auto folder = base + "Tables" + PathSeparator;
		std::vector<std::string> names;
#ifdef _WIN32
		auto pattern = folder + "*.pwtable";
		int wideLen = MultiByteToWideChar(CP_UTF8, 0, pattern.c_str(), -1, nullptr, 0);
		std::wstring wide(static_cast<size_t>(wideLen), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, pattern.c_str(), -1, &wide[0], wideLen);
		WIN32_FIND_DATAW data;
		auto handle = FindFirstFileW(wide.c_str(), &data);
		if (handle != INVALID_HANDLE_VALUE)
		{
			do
			{
				if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					continue;
				int len = WideCharToMultiByte(CP_UTF8, 0, data.cFileName, -1, nullptr, 0, nullptr, nullptr);
				std::string name(static_cast<size_t>(len), '\0');
				WideCharToMultiByte(CP_UTF8, 0, data.cFileName, -1, &name[0], len, nullptr, nullptr);
				name.resize(strlen(name.c_str()));
				names.push_back(name);
			}
			while (FindNextFileW(handle, &data));
			FindClose(handle);
		}
#else
		auto dir = opendir(folder.c_str());
		if (dir)
		{
			while (auto ent = readdir(dir))
			{
				std::string name = ent->d_name;
				if (name.size() > 8 && name.compare(name.size() - 8, 8, ".pwtable") == 0)
					names.push_back(name);
			}
			closedir(dir);
		}
#endif
		std::sort(names.begin(), names.end());
		for (auto& name : names)
		{
			if (std::find(seen.begin(), seen.end(), name) != seen.end())
				continue;
			CustomTableEntry entry;
			if (ReadInfo(folder + name, entry))
			{
				seen.push_back(name);
				result.push_back(entry);
			}
		}
	}
	return result;
}

std::string customtable::ResolveStartTable(const std::string& saved, bool haveSpaceCadet)
{
	if (!saved.empty())
	{
		auto file = fopenu(saved.c_str(), "rb");
		if (file)
		{
			fclose(file);
			return saved;
		}
		// The game folder may have moved: look for a table with the same file name.
		auto slash = saved.find_last_of("/\\");
		auto fileName = slash == std::string::npos ? saved : saved.substr(slash + 1);
		for (auto& folder : SearchFolders)
		{
			auto candidate = folder + "Tables" + PathSeparator + fileName;
			file = fopenu(candidate.c_str(), "rb");
			if (file)
			{
				fclose(file);
				return candidate;
			}
		}
	}
	if (!haveSpaceCadet)
	{
		auto tables = FindTables(SearchFolders);
		if (!tables.empty())
			return tables[0].Path;
	}
	return "";
}
