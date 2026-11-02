#include "pch.h"
#include "tableeditor.h"

#include "customtable.h"
#include "minijson.h"
#include "options.h"
#include "tableart.h"
#include "pb.h"
#include "winmain.h"
#include "stb_image.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

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
#include <sys/stat.h>
#endif

using minijson::Value;

bool tableeditor::Open = false;

namespace
{
	Value Doc;
	std::string FilePath;
	bool Dirty = false;
	int Selected = -1;
	int SelectedPoint = -1;
	std::vector<std::string> UndoStack, RedoStack;
	std::string PendingBase;
	bool Pending = false;
	bool Snap = true;
	float Zoom = 1.0f;
	ImVec2 Pan{0, 0};
	ImVec2 CanvasCenter{0, 0};
	float CanvasScale = 10;
	std::string Status;
	double StatusUntil = 0;
	bool AskClose = false;
	bool SkipChange = false; // the document changed by undo or redo, not by an edit
	bool RulesTab = false;
	int PickGroup = -1;      // rules group that clicks on the table add parts to
	int HighlightGroup = -1; // rules group whose parts are outlined on the table
	int OpenGroup = -1;      // rules group to unfold on the next frame
	SDL_Texture* PictureTexture = nullptr;
	std::string PictureLoaded; // file the picture texture was made from

	struct ThemePreset
	{
		const char* Name;
		const char* Top;
		const char* Bottom;
		const char* Walls;
		const char* Rubber;
		const char* Accent;
		const char* Accent2;
		const char* Cabinet;
		const char* Panel;
	};

	// Ready-made colour sets. The playfield runs from Top (far end) to Bottom (flippers).
	const ThemePreset Themes[] = {
		{"Midnight", "#1b2a55", "#0b1026", "#b9c6dd", "#f2f2ee", "#ff8a1f", "#33ccff", "#1e1826", "#0d0f1c"},
		{"Deep Space", "#151a3d", "#04050d", "#a9b8ff", "#e8ecff", "#ff4fd8", "#4dfcff", "#121226", "#07070f"},
		{"Sunset", "#ff8a4c", "#4a1650", "#ffe4c8", "#fff4e6", "#ffd23f", "#ff3d7f", "#2b1030", "#170816"},
		{"Jungle", "#24663c", "#0a1f12", "#d9e8b0", "#f4f8e8", "#ffb300", "#7cff6b", "#2a1d10", "#0b140c"},
		{"Neon Arcade", "#1a0033", "#020005", "#e4e0ff", "#ffffff", "#ff2bd6", "#00f0ff", "#0d0d18", "#05050a"},
		{"Skatepark", "#9aa0a6", "#50565c", "#f4f4f4", "#ffffff", "#ff5a1f", "#2ee6a8", "#23262b", "#121417"},
		{"Candy", "#ffb7d5", "#a46cff", "#ffffff", "#ffffff", "#ff3d7f", "#3dd6ff", "#5a2a6e", "#2a1236"},
		{"Fire", "#8a1a08", "#1a0402", "#ffd0a0", "#fff0e0", "#ffcc00", "#ff5a00", "#2a0c06", "#140402"},
		{"Ice", "#bfe6ff", "#2a5a8a", "#ffffff", "#ffffff", "#2a7fff", "#00e0ff", "#1a2a40", "#0a1420"},
	};

	enum class DragMode { None, Body, Point, View };
	DragMode Dragging = DragMode::None;
	ImVec2 DragStart{0, 0};
	Value DragOriginal;

	const float Pi = 3.14159265f;

	void SetStatus(const std::string& text)
	{
		Status = text;
		StatusUntil = ImGui::GetTime() + 4.0;
	}

	// ---------------------------------------------------------------------------------------------
	// Table data helpers

	Value& Parts()
	{
		auto& parts = Doc.At("parts");
		if (!parts.IsArray())
			parts = Value::MakeArray();
		return parts;
	}

	float TableW() { return Doc["playfield"]["width"].Float(16.0f); }
	float TableL() { return Doc["playfield"]["length"].Float(29.0f); }

	bool HasPoints(const std::string& type)
	{
		return type == "wall" || type == "slingshot" || type == "lane" || type == "ramp" || type == "gate";
	}

	float SnapValue(float v)
	{
		return Snap ? std::round(v * 10.0f) / 10.0f : std::round(v * 1000.0f) / 1000.0f;
	}

	void SetNum(Value& v, float n) { v = Value(static_cast<double>(std::round(n * 1000.0f) / 1000.0f)); }

	Value Point(float x, float y)
	{
		auto p = Value::MakeArray();
		p.Push(Value(x));
		p.Push(Value(y));
		return p;
	}

	// Moves a part by dx, dy from its original state.
	void MovePart(Value& part, const Value& original, float dx, float dy)
	{
		auto type = original["type"].Str();
		if (HasPoints(type))
		{
			auto& pts = part.At("points").MutableItems();
			auto& src = original["points"].Items();
			for (size_t i = 0; i < pts.size() && i < src.size(); i++)
			{
				pts[i] = Point(SnapValue(src[i][static_cast<size_t>(0)].Float() + dx),
				               SnapValue(src[i][static_cast<size_t>(1)].Float() + dy));
			}
		}
		else if (type == "drain")
		{
			SetNum(part.At("y"), SnapValue(original["y"].Float(TableL() / 2 - 0.3f) + dy));
			SetNum(part.At("x0"), SnapValue(original["x0"].Float(-TableW() / 2) + dx));
			SetNum(part.At("x1"), SnapValue(original["x1"].Float(TableW() / 2) + dx));
		}
		else
		{
			SetNum(part.At("x"), SnapValue(original["x"].Float() + dx));
			SetNum(part.At("y"), SnapValue(original["y"].Float() + dy));
		}
	}

	std::string UniqueId(const std::string& base)
	{
		for (int n = 1;; n++)
		{
			auto id = base + " " + std::to_string(n);
			bool used = false;
			for (auto& part : Parts().Items())
				if (part["id"].Str() == id)
					used = true;
			if (!used)
				return id;
		}
	}

	int CountType(const std::string& type, const std::string& side = "")
	{
		int count = 0;
		for (auto& part : Parts().Items())
			if (part["type"].Str() == type && (side.empty() || part["side"].Str("left") == side))
				count++;
		return count;
	}

	// Rules refer to parts by name; keep them in step when a part is renamed or deleted.
	void RenameRefs(const std::string& from, const std::string& to)
	{
		if (!Doc.Has("rules"))
			return;
		auto& rules = Doc.At("rules");
		if (!rules.IsObject())
			return;
		auto fix = [&](Value& list)
		{
			if (!list.IsArray())
				return;
			for (auto& item : list.MutableItems())
				if (item.Str() == from)
					item = Value(to);
		};
		if (rules.Has("groups"))
		{
			for (auto& group : rules.At("groups").MutableItems())
			{
				if (group.Has("parts"))
					fix(group.At("parts"));
				if (group.Has("lights"))
					fix(group.At("lights"));
			}
		}
		if (rules["extraBallLight"].Str() == from)
			rules.At("extraBallLight") = Value(to);
		if (rules.Has("multiplierLights"))
			fix(rules.At("multiplierLights"));
	}

	void RemoveRefs(const std::string& id)
	{
		if (!Doc.Has("rules"))
			return;
		auto& rules = Doc.At("rules");
		if (!rules.IsObject())
			return;
		auto drop = [&](Value& list)
		{
			if (!list.IsArray())
				return;
			auto& items = list.MutableItems();
			for (size_t i = items.size(); i-- > 0;)
				if (items[i].Str() == id)
					items.erase(items.begin() + i);
		};
		if (rules.Has("groups"))
		{
			auto& groups = rules.At("groups").MutableItems();
			for (size_t g = groups.size(); g-- > 0;)
			{
				if (groups[g].Has("parts"))
					drop(groups[g].At("parts"));
				if (groups[g].Has("lights"))
					drop(groups[g].At("lights"));
				if (!groups[g]["parts"].Size())
					groups.erase(groups.begin() + g);
			}
		}
		if (rules["extraBallLight"].Str() == id)
			rules.Remove("extraBallLight");
		if (rules.Has("multiplierLights"))
			drop(rules.At("multiplierLights"));
	}

	bool IdUsed(const std::string& id, int except)
	{
		auto& items = Parts().Items();
		for (size_t i = 0; i < items.size(); i++)
			if (static_cast<int>(i) != except && items[i]["id"].Str() == id)
				return true;
		return false;
	}

	// ---------------------------------------------------------------------------------------------
	// Pictures

	std::string TableFolder()
	{
		auto cut = FilePath.find_last_of("/\\");
		return cut == std::string::npos ? std::string() : FilePath.substr(0, cut + 1);
	}

	bool IsPicture(std::string name)
	{
		for (auto& c : name)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		auto ends = [&](const char* ext)
		{
			auto n = strlen(ext);
			return name.size() > n && name.compare(name.size() - n, n, ext) == 0;
		};
		return ends(".png") || ends(".jpg") || ends(".jpeg");
	}

	// Pictures in the table's folder, by file name.
	std::vector<std::string> ListPictures()
	{
		std::vector<std::string> names;
		auto folder = TableFolder();
#ifdef _WIN32
		auto pattern = folder + "*";
		int len = MultiByteToWideChar(CP_UTF8, 0, pattern.c_str(), -1, nullptr, 0);
		std::wstring wide(static_cast<size_t>(len > 0 ? len : 1), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, pattern.c_str(), -1, &wide[0], len);
		WIN32_FIND_DATAW data;
		auto handle = FindFirstFileW(wide.c_str(), &data);
		if (handle != INVALID_HANDLE_VALUE)
		{
			do
			{
				if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					continue;
				int n = WideCharToMultiByte(CP_UTF8, 0, data.cFileName, -1, nullptr, 0, nullptr, nullptr);
				std::string name(static_cast<size_t>(n), '\0');
				WideCharToMultiByte(CP_UTF8, 0, data.cFileName, -1, &name[0], n, nullptr, nullptr);
				name.resize(strlen(name.c_str()));
				if (IsPicture(name))
					names.push_back(name);
			}
			while (FindNextFileW(handle, &data));
			FindClose(handle);
		}
#else
		auto dir = opendir(folder.empty() ? "." : folder.c_str());
		if (dir)
		{
			while (auto ent = readdir(dir))
				if (IsPicture(ent->d_name))
					names.emplace_back(ent->d_name);
			closedir(dir);
		}
#endif
		std::sort(names.begin(), names.end());
		return names;
	}

	void FreePicture()
	{
		if (PictureTexture)
			SDL_DestroyTexture(PictureTexture);
		PictureTexture = nullptr;
		PictureLoaded.clear();
	}

	// Texture of the playfield picture for the editor view, or nullptr.
	SDL_Texture* Picture()
	{
		auto name = Doc["playfield"]["image"].Str();
		if (name == PictureLoaded)
			return PictureTexture;
		FreePicture();
		PictureLoaded = name;
		if (name.empty() || !winmain::Renderer)
			return nullptr;
		auto file = fopenu((TableFolder() + name).c_str(), "rb");
		if (!file)
			return nullptr;
		std::string bytes;
		char buffer[65536];
		size_t got;
		while ((got = fread(buffer, 1, sizeof buffer, file)) > 0)
			bytes.append(buffer, got);
		fclose(file);
		int w, h, channels;
		auto pixels = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(bytes.data()),
		                                    static_cast<int>(bytes.size()), &w, &h, &channels, 4);
		if (!pixels)
			return nullptr;
		PictureTexture = SDL_CreateTexture(winmain::Renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
		if (PictureTexture)
			SDL_UpdateTexture(PictureTexture, nullptr, pixels, w * 4);
		stbi_image_free(pixels);
		return PictureTexture;
	}

	// ---------------------------------------------------------------------------------------------
	// Files

	bool WriteFile(const std::string& path, const std::string& text)
	{
		auto file = fopenu(path.c_str(), "wb");
		if (!file)
			return false;
		auto written = fwrite(text.data(), 1, text.size(), file);
		fclose(file);
		return written == text.size();
	}

	bool FileExists(const std::string& path)
	{
		auto file = fopenu(path.c_str(), "rb");
		if (!file)
			return false;
		fclose(file);
		return true;
	}

	void MakeFolder(const std::string& path)
	{
#ifdef _WIN32
		int len = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
		std::wstring wide(static_cast<size_t>(len > 0 ? len : 1), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wide[0], len);
		CreateDirectoryW(wide.c_str(), nullptr);
#else
		mkdir(path.c_str(), 0755);
#endif
	}

	bool Save()
	{
		Doc.Set("format", Value("pinball-wizz-table"));
		Doc.Set("version", Value(1));
		if (!WriteFile(FilePath, Doc.Dump(2) + "\n"))
		{
			SetStatus("Couldn't save the table file");
			return false;
		}
		Dirty = false;
		SetStatus("Saved");
		return true;
	}

	void Close()
	{
		FreePicture();
		PickGroup = HighlightGroup = -1;
		tableeditor::Open = false;
		AskClose = false;
		Dragging = DragMode::None;
		winmain::end_pause();
	}

	void Undo()
	{
		if (UndoStack.empty())
			return;
		RedoStack.push_back(Doc.Dump());
		std::string error;
		minijson::Parse(UndoStack.back(), Doc, error);
		UndoStack.pop_back();
		SkipChange = true;
		Selected = std::min(Selected, static_cast<int>(Parts().Size()) - 1);
		SelectedPoint = -1;
		Dirty = true;
		Pending = false;
	}

	void Redo()
	{
		if (RedoStack.empty())
			return;
		UndoStack.push_back(Doc.Dump());
		std::string error;
		minijson::Parse(RedoStack.back(), Doc, error);
		RedoStack.pop_back();
		SkipChange = true;
		Selected = std::min(Selected, static_cast<int>(Parts().Size()) - 1);
		SelectedPoint = -1;
		Dirty = true;
		Pending = false;
	}

	// ---------------------------------------------------------------------------------------------
	// Starter table

	Value StarterTable(const std::string& name)
	{
		auto doc = Value::MakeObject();
		doc.Set("format", Value("pinball-wizz-table"));
		doc.Set("version", Value(1));
		doc.Set("name", Value(name));
		doc.Set("author", Value(""));
		auto playfield = Value::MakeObject();
		playfield.Set("width", Value(16));
		playfield.Set("length", Value(29));
		playfield.Set("color", Value("#1b3a55"));
		playfield.Set("color2", Value("#0a1426"));
		doc.Set("playfield", playfield);
		doc.Set("ballsPerGame", Value(3));

		auto parts = Value::MakeArray();
		auto add = [&](std::initializer_list<std::pair<const char*, Value>> fields)
		{
			auto part = Value::MakeObject();
			for (auto& field : fields)
				part.Set(field.first, field.second);
			parts.Push(part);
		};
		auto points = [](std::initializer_list<std::pair<float, float>> list)
		{
			auto pts = Value::MakeArray();
			for (auto& p : list)
				pts.Push(Point(p.first, p.second));
			return pts;
		};

		// Outer wall: up the left side, round the top, down the right side.
		auto outer = Value::MakeArray();
		outer.Push(Point(-7.6f, 14.5f));
		for (int i = 0; i <= 24; i++)
		{
			auto a = Pi + Pi * i / 24;
			outer.Push(Point(std::round(7.6f * std::cos(a) * 1000) / 1000,
			                 std::round((-7.0f + 7.0f * std::sin(a)) * 1000) / 1000));
		}
		outer.Push(Point(7.6f, 14.5f));
		add({{"type", Value("wall")}, {"id", Value("outer wall")}, {"points", outer}});
		add({{"type", Value("wall")}, {"id", Value("plunger lane wall")}, {"points", points({{6.6f, 14.5f}, {6.6f, -6}})}});
		add({{"type", Value("wall")}, {"id", Value("left guide")}, {"points", points({{-7.6f, 7.5f}, {-3.35f, 11.75f}})}});
		add({{"type", Value("wall")}, {"id", Value("right guide")}, {"points", points({{6.6f, 7.5f}, {2.35f, 11.75f}})}});
		add({{"type", Value("slingshot")}, {"id", Value("left slingshot")},
			{"points", points({{-6, 6.6f}, {-6, 8.9f}, {-4.4f, 10.3f}})}});
		add({{"type", Value("slingshot")}, {"id", Value("right slingshot")},
			{"points", points({{5, 6.6f}, {3.4f, 10.3f}, {5, 8.9f}})}});
		add({{"type", Value("bumper")}, {"id", Value("bumper 1")}, {"x", Value(-2.5f)}, {"y", Value(-6.0f)}});
		add({{"type", Value("bumper")}, {"id", Value("bumper 2")}, {"x", Value(1.5f)}, {"y", Value(-6.0f)}});
		add({{"type", Value("bumper")}, {"id", Value("bumper 3")}, {"x", Value(-0.5f)}, {"y", Value(-3.4f)}});
		add({{"type", Value("flipper")}, {"side", Value("left")}, {"x", Value(-3.1f)}, {"y", Value(12.0f)},
			{"length", Value(2.2f)}});
		add({{"type", Value("flipper")}, {"side", Value("right")}, {"x", Value(2.1f)}, {"y", Value(12.0f)},
			{"length", Value(2.2f)}});
		add({{"type", Value("plunger")}, {"x", Value(7.1f)}, {"y", Value(13.2f)}, {"width", Value(0.7f)}});
		add({{"type", Value("drain")}, {"y", Value(14.25f)}, {"x0", Value(-7.6f)}, {"x1", Value(6.6f)}});
		doc.Set("parts", parts);
		return doc;
	}

	// ---------------------------------------------------------------------------------------------
	// Drawing and picking

	ImVec2 ToScreen(float x, float y)
	{
		return ImVec2(CanvasCenter.x + Pan.x + x * CanvasScale, CanvasCenter.y + Pan.y + y * CanvasScale);
	}

	ImVec2 ToTable(const ImVec2& p)
	{
		return ImVec2((p.x - CanvasCenter.x - Pan.x) / CanvasScale, (p.y - CanvasCenter.y - Pan.y) / CanvasScale);
	}

	float SegmentDistance(const ImVec2& p, const ImVec2& a, const ImVec2& b)
	{
		auto dx = b.x - a.x, dy = b.y - a.y;
		auto len2 = dx * dx + dy * dy;
		auto t = len2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0.0f;
		t = std::max(0.0f, std::min(1.0f, t));
		auto cx = a.x + dx * t - p.x, cy = a.y + dy * t - p.y;
		return std::sqrt(cx * cx + cy * cy);
	}

	bool Inside(const ImVec2& p, const std::vector<ImVec2>& poly)
	{
		bool in = false;
		for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
		{
			if ((poly[i].y > p.y) != (poly[j].y > p.y) &&
				p.x < (poly[j].x - poly[i].x) * (p.y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x)
				in = !in;
		}
		return in;
	}

	std::vector<ImVec2> ShapePoints(const TablePreviewShape& shape)
	{
		std::vector<ImVec2> pts;
		for (size_t i = 0; i + 1 < shape.Points.size(); i += 2)
			pts.emplace_back(shape.Points[i], shape.Points[i + 1]);
		return pts;
	}

	int PickPart(const TablePreview& preview, const ImVec2& tablePos)
	{
		auto tolerance = 6.0f / CanvasScale;
		for (auto it = preview.Shapes.rbegin(); it != preview.Shapes.rend(); ++it)
		{
			auto pts = ShapePoints(*it);
			if (pts.empty())
				continue;
			if (it->Kind == TablePreviewShape::Circle)
			{
				auto dx = tablePos.x - pts[0].x, dy = tablePos.y - pts[0].y;
				if (std::sqrt(dx * dx + dy * dy) <= it->Radius + tolerance)
					return it->PartIndex;
				continue;
			}
			if (it->Filled && pts.size() >= 3 && Inside(tablePos, pts))
				return it->PartIndex;
			for (size_t i = 0; i + 1 < pts.size(); i++)
				if (SegmentDistance(tablePos, pts[i], pts[i + 1]) <= it->Width * 0.5f + tolerance)
					return it->PartIndex;
			if (it->Filled && pts.size() >= 3 && SegmentDistance(tablePos, pts.back(), pts[0]) <= tolerance)
				return it->PartIndex;
		}
		return -1;
	}

	void DrawShape(ImDrawList* draw, const TablePreviewShape& shape, ImU32 color, float extra)
	{
		std::vector<ImVec2> pts;
		for (size_t i = 0; i + 1 < shape.Points.size(); i += 2)
			pts.push_back(ToScreen(shape.Points[i], shape.Points[i + 1]));
		if (pts.empty())
			return;
		if (shape.Kind == TablePreviewShape::Circle)
		{
			draw->AddCircleFilled(pts[0], std::max(2.0f, shape.Radius * CanvasScale) + extra, color, 24);
			return;
		}
		if (shape.Filled && pts.size() >= 3 && extra == 0)
		{
			draw->AddConvexPolyFilled(pts.data(), static_cast<int>(pts.size()), color);
			return;
		}
		auto width = std::max(1.5f, shape.Width * CanvasScale) + extra * 2;
		draw->AddPolyline(pts.data(), static_cast<int>(pts.size()), color,
		                  shape.Filled ? ImDrawFlags_Closed : 0, width);
		for (auto& p : pts)
			draw->AddCircleFilled(p, width * 0.5f, color, 10);
	}

	// Handles of the selected part: its points, or its centre.
	std::vector<ImVec2> Handles(const Value& part)
	{
		std::vector<ImVec2> handles;
		auto type = part["type"].Str();
		if (HasPoints(type))
		{
			for (auto& p : part["points"].Items())
				handles.emplace_back(p[static_cast<size_t>(0)].Float(), p[static_cast<size_t>(1)].Float());
		}
		return handles;
	}

	// ---------------------------------------------------------------------------------------------
	// Settings panel

	bool EditFloat(Value& part, const char* key, const char* label, float def, float minV, float maxV,
	               float speed = 0.02f)
	{
		auto v = part[key].Float(def);
		ImGui::SetNextItemWidth(-90);
		if (ImGui::DragFloat(label, &v, speed, minV, maxV, "%.2f", ImGuiSliderFlags_AlwaysClamp))
		{
			SetNum(part.At(key), v);
			return true;
		}
		return false;
	}

	bool EditInt(Value& part, const char* key, const char* label, int def, int minV, int maxV, float speed = 10)
	{
		auto v = part[key].Int(def);
		ImGui::SetNextItemWidth(-90);
		if (ImGui::DragInt(label, &v, speed, minV, maxV, "%d", ImGuiSliderFlags_AlwaysClamp))
		{
			part.At(key) = Value(v);
			return true;
		}
		return false;
	}

	bool EditColor(Value& part, const char* key, const char* label, const char* def)
	{
		auto c = ArtColor::FromHex(part[key].Str(def), {0.8f, 0.8f, 0.8f});
		float rgb[3] = {c.R, c.G, c.B};
		if (ImGui::ColorEdit3(label, rgb, ImGuiColorEditFlags_NoInputs))
		{
			char hex[8];
			auto ch = [](float v) { return static_cast<int>(std::max(0.0f, std::min(1.0f, v)) * 255.0f + 0.5f); };
			snprintf(hex, sizeof hex, "#%02x%02x%02x", ch(rgb[0]), ch(rgb[1]), ch(rgb[2]));
			part.At(key) = Value(hex);
			return true;
		}
		return false;
	}

	bool EditText(Value& part, const char* key, const char* label)
	{
		char buffer[128];
		snprintf(buffer, sizeof buffer, "%s", part[key].Str().c_str());
		ImGui::SetNextItemWidth(-90);
		if (ImGui::InputText(label, buffer, sizeof buffer))
		{
			part.At(key) = Value(std::string(buffer));
			return true;
		}
		return false;
	}

	// A part's name is used by the rules, so it changes when typing finishes, and rules follow it.
	void EditId(Value& part)
	{
		static char buffer[128];
		static int editing = -2;
		if (editing != Selected)
			snprintf(buffer, sizeof buffer, "%s", part["id"].Str().c_str());
		ImGui::SetNextItemWidth(-90);
		ImGui::InputText("Name", buffer, sizeof buffer);
		auto active = ImGui::IsItemActive();
		if (ImGui::IsItemDeactivatedAfterEdit())
		{
			std::string from = part["id"].Str(), to = buffer;
			if (to.empty())
				SetStatus("A part needs a name");
			else if (IdUsed(to, Selected))
				SetStatus("Another part already has that name");
			else if (to != from)
			{
				part.At("id") = Value(to);
				if (!from.empty() && !IdUsed(from, Selected))
					RenameRefs(from, to);
			}
		}
		if (!active)
			snprintf(buffer, sizeof buffer, "%s", part["id"].Str().c_str());
		editing = Selected;
	}

	bool EditChoice(Value& part, const char* key, const char* label, std::initializer_list<const char*> choices)
	{
		auto current = part[key].Str(*choices.begin());
		bool changed = false;
		ImGui::SetNextItemWidth(-90);
		if (ImGui::BeginCombo(label, current.c_str()))
		{
			for (auto choice : choices)
			{
				if (ImGui::Selectable(choice, current == choice))
				{
					part.At(key) = Value(choice);
					changed = true;
				}
			}
			ImGui::EndCombo();
		}
		return changed;
	}

	void EditPosition(Value& part)
	{
		EditFloat(part, "x", "Across", 0, -40, 40, 0.05f);
		EditFloat(part, "y", "Down", 0, -40, 40, 0.05f);
	}

	void PartSettings(Value& part)
	{
		auto type = part["type"].Str();
		ImGui::TextColored(ImVec4(0.75f, 0.65f, 1.0f, 1.0f), "%c%s", std::toupper(type.empty() ? '?' : type[0]),
		                   type.size() > 1 ? type.substr(1).c_str() : "");
		ImGui::Separator();
		EditId(part);

		if (type == "wall")
		{
			EditFloat(part, "thickness", "Thickness", 0.3f, 0.05f, 2.0f);
			EditFloat(part, "height", "Height", 0.55f, 0.1f, 2.0f);
			auto closed = part["closed"].Bool(false);
			if (ImGui::Checkbox("Solid shape", &closed))
				part.At("closed") = Value(closed);
			EditColor(part, "color", "Colour", "#b9c6dd");
			EditInt(part, "score", "Score", 0, 0, 100000);
		}
		else if (type == "post")
		{
			EditPosition(part);
			EditFloat(part, "radius", "Size", 0.2f, 0.1f, 1.0f);
			EditInt(part, "score", "Score", 0, 0, 100000);
		}
		else if (type == "bumper")
		{
			EditPosition(part);
			EditFloat(part, "radius", "Size", 0.55f, 0.3f, 1.5f);
			EditFloat(part, "strength", "Kick", 1.0f, 0.2f, 3.0f);
			EditColor(part, "color", "Body", "#ff8a1f");
			ImGui::SameLine();
			EditColor(part, "capColor", "Cap", "#33ccff");
			EditInt(part, "score", "Score", 100, 0, 100000);
		}
		else if (type == "slingshot")
		{
			EditFloat(part, "strength", "Kick", 1.0f, 0.2f, 3.0f);
			EditColor(part, "color", "Colour", "#ff8a1f");
			EditInt(part, "score", "Score", 10, 0, 100000);
		}
		else if (type == "flipper")
		{
			EditChoice(part, "side", "Side", {"left", "right"});
			EditPosition(part);
			EditFloat(part, "length", "Length", 2.0f, 1.0f, 4.0f);
			EditFloat(part, "angle", "Rest angle", 30.0f, -60.0f, 80.0f, 0.5f);
			EditFloat(part, "swing", "Swing", 55.0f, 10.0f, 90.0f, 0.5f);
			EditFloat(part, "strength", "Power", 1.0f, 0.3f, 2.0f);
			EditColor(part, "color", "Colour", "#f2f2f7");
		}
		else if (type == "plunger")
		{
			EditPosition(part);
			EditFloat(part, "width", "Width", 0.7f, 0.4f, 1.5f);
		}
		else if (type == "drain")
		{
			EditFloat(part, "y", "Down", TableL() / 2 - 0.3f, -40, 40, 0.05f);
			EditFloat(part, "x0", "Left end", -TableW() / 2, -40, 40, 0.05f);
			EditFloat(part, "x1", "Right end", TableW() / 2, -40, 40, 0.05f);
		}
		else if (type == "lane")
		{
			EditInt(part, "score", "Score", 100, 0, 100000);
			EditInt(part, "bonus", "Bonus", 500, 0, 100000);
		}
		else if (type == "target")
		{
			EditPosition(part);
			EditFloat(part, "width", "Width", 0.8f, 0.3f, 2.0f);
			EditFloat(part, "angle", "Facing", 90.0f, -180.0f, 180.0f, 1.0f);
			EditColor(part, "color", "Colour", "#ff8a1f");
			EditInt(part, "score", "Score", 250, 0, 100000);
			EditInt(part, "bonus", "Bonus", 500, 0, 100000);
		}
		else if (type == "drop")
		{
			EditPosition(part);
			EditFloat(part, "width", "Width", 0.8f, 0.3f, 2.0f);
			EditFloat(part, "angle", "Facing", 90.0f, -180.0f, 180.0f, 1.0f);
			EditFloat(part, "popup", "Pops up after", 1.0f, 0.2f, 10.0f, 0.05f);
			EditColor(part, "color", "Colour", "#ff8a1f");
			EditInt(part, "score", "Score", 500, 0, 100000);
			EditInt(part, "bonus", "Bonus", 1000, 0, 100000);
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::TextWrapped("Drops when hit. Put drop targets in a rules group: they come back up when the "
			                   "group is done.");
			ImGui::PopStyleColor();
		}
		else if (type == "kickout")
		{
			EditPosition(part);
			EditFloat(part, "radius", "Size", 0.45f, 0.3f, 1.0f);
			EditFloat(part, "angle", "Kicks towards", 90.0f, -180.0f, 180.0f, 1.0f);
			EditFloat(part, "strength", "Kick", 1.0f, 0.3f, 3.0f);
			EditColor(part, "color", "Colour", "#ff8a1f");
			EditInt(part, "score", "Score", 2500, 0, 100000);
			EditInt(part, "bonus", "Bonus", 1000, 0, 100000);
		}
		else if (type == "spinner")
		{
			EditPosition(part);
			EditFloat(part, "width", "Width", 1.0f, 0.5f, 2.5f);
			EditFloat(part, "angle", "Turn", 0.0f, -180.0f, 180.0f, 1.0f);
			EditColor(part, "color", "Colour", "#33ccff");
			EditInt(part, "score", "Score per spin", 100, 0, 100000);
		}
		else if (type == "ramp")
		{
			EditFloat(part, "width", "Width", 1.1f, 0.8f, 2.5f);
			EditColor(part, "color", "Floor", "#33ccff");
			EditColor(part, "railColor", "Rails", "#c8d0dc");
			EditInt(part, "score", "Score", 5000, 0, 1000000);
			EditInt(part, "bonus", "Bonus", 2000, 0, 1000000);
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::TextWrapped("The ball enters at the first point and scores when it leaves the last one.");
			ImGui::PopStyleColor();
		}
		else if (type == "gate")
		{
			EditColor(part, "color", "Colour", "#c8d0dc");
			EditInt(part, "score", "Score", 0, 0, 100000);
			if (ImGui::Button("Flip direction"))
			{
				auto& pts = part.At("points").MutableItems();
				if (pts.size() == 2)
					std::swap(pts[0], pts[1]);
			}
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::TextWrapped("A one-way flap. The ball passes from the side the arrow comes from and is "
			                   "stopped the other way.");
			ImGui::PopStyleColor();
		}
		else if (type == "light")
		{
			EditPosition(part);
			EditFloat(part, "radius", "Size", 0.35f, 0.15f, 1.0f);
			EditChoice(part, "shape", "Shape", {"circle", "arrow"});
			if (part["shape"].Str() == "arrow")
				EditFloat(part, "angle", "Points", -90.0f, -180.0f, 180.0f, 1.0f);
			EditColor(part, "color", "Colour", "#ff8a1f");
			EditText(part, "label", "Label");
		}

		if (HasPoints(type))
		{
			ImGui::Spacing();
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::TextWrapped("Drag the white points on the table to reshape it.");
			ImGui::PopStyleColor();
			if (type == "wall" || type == "ramp")
			{
				auto& pts = part.At("points").MutableItems();
				if (SelectedPoint >= 0 && SelectedPoint < static_cast<int>(pts.size()))
				{
					if (ImGui::Button("Add point"))
					{
						auto next = SelectedPoint + 1 < static_cast<int>(pts.size()) ? SelectedPoint + 1 : SelectedPoint - 1;
						auto& a = pts[SelectedPoint];
						auto& b = pts[std::max(0, next)];
						auto mx = (a[static_cast<size_t>(0)].Float() + b[static_cast<size_t>(0)].Float()) / 2;
						auto my = (a[static_cast<size_t>(1)].Float() + b[static_cast<size_t>(1)].Float()) / 2;
						if (next == SelectedPoint - 1)
						{
							mx = a[static_cast<size_t>(0)].Float() + 0.5f;
							my = a[static_cast<size_t>(1)].Float();
						}
						pts.insert(pts.begin() + SelectedPoint + 1, Point(SnapValue(mx), SnapValue(my)));
						SelectedPoint++;
					}
					ImGui::SameLine();
					if (pts.size() > 2 && ImGui::Button("Remove point"))
					{
						pts.erase(pts.begin() + SelectedPoint);
						SelectedPoint = -1;
					}
				}
				else
				{
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
					ImGui::TextWrapped("Click a point to add or remove points.");
					ImGui::PopStyleColor();
				}
			}
		}
	}

	void TableSettings()
	{
		ImGui::TextColored(ImVec4(0.75f, 0.65f, 1.0f, 1.0f), "Table");
		ImGui::Separator();
		EditText(Doc, "name", "Name");
		EditText(Doc, "author", "Made by");
		EditInt(Doc, "ballsPerGame", "Balls", 3, 1, 9, 0.05f);
		ImGui::Spacing();
		ImGui::TextColored(ImVec4(0.75f, 0.65f, 1.0f, 1.0f), "Look");
		ImGui::Separator();
		auto& playfield = Doc.At("playfield");
		auto& theme = Doc.At("theme");

		auto themeName = theme["name"].Str("Custom");
		ImGui::SetNextItemWidth(-90);
		if (ImGui::BeginCombo("Theme", themeName.c_str()))
		{
			for (auto& preset : Themes)
			{
				if (ImGui::Selectable(preset.Name, themeName == preset.Name))
				{
					theme.At("name") = Value(preset.Name);
					playfield.At("color") = Value(preset.Top);
					playfield.At("color2") = Value(preset.Bottom);
					theme.At("walls") = Value(preset.Walls);
					theme.At("rubber") = Value(preset.Rubber);
					theme.At("accent") = Value(preset.Accent);
					theme.At("accent2") = Value(preset.Accent2);
					theme.At("cabinet") = Value(preset.Cabinet);
					theme.At("panel") = Value(preset.Panel);
				}
			}
			ImGui::EndCombo();
		}

		auto image = playfield["image"].Str();
		ImGui::SetNextItemWidth(-90);
		if (ImGui::BeginCombo("Picture", image.empty() ? "None (painted)" : image.c_str()))
		{
			if (ImGui::Selectable("None (painted)", image.empty()))
				playfield.Remove("image");
			auto pictures = ListPictures();
			for (auto& name : pictures)
				if (ImGui::Selectable(name.c_str(), name == image))
					playfield.At("image") = Value(name);
			if (pictures.empty())
				ImGui::TextDisabled("No pictures in the table's folder");
			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Put a PNG or JPG next to the table file to use it as the playfield.\n"
			                  "Tall pictures fit best (about 16 by 29).");
		if (image.empty())
		{
			// Pattern painted under the parts when there is no picture.
			auto decor = playfield["decor"].Str("grid");
			ImGui::SetNextItemWidth(-90);
			if (ImGui::BeginCombo("Pattern", decor == "space" ? "Stars and planet" : "Grid"))
			{
				if (ImGui::Selectable("Grid", decor != "space"))
					playfield.At("decor") = Value("grid");
				if (ImGui::Selectable("Stars and planet", decor == "space"))
					playfield.At("decor") = Value("space");
				ImGui::EndCombo();
			}
			if (playfield["decor"].Str() == "space")
			{
				EditFloat(playfield, "planetX", "Planet across", 0.0f, -TableW() / 2, TableW() / 2, 0.05f);
				EditFloat(playfield, "planetY", "Planet down", 1.0f, -TableL() / 2, TableL() / 2, 0.05f);
			}
		}
		ImGui::Spacing();

		// Changing one colour by hand makes the theme a custom one.
		auto before = Doc.Dump();
		EditColor(playfield, "color", "Playfield top", "#1b2a55");
		EditColor(playfield, "color2", "Playfield bottom", "#0b1026");
		EditColor(theme, "walls", "Walls", "#b9c6dd");
		EditColor(theme, "accent", "Accent", "#ff8a1f");
		EditColor(theme, "accent2", "Second accent", "#33ccff");
		EditColor(theme, "cabinet", "Cabinet", "#1e1826");
		EditColor(theme, "panel", "Score panel", "#0d0f1c");
		if (Doc.Dump() != before)
			theme.At("name") = Value("Custom");
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::TextWrapped("Click a part on the table to change it. Delete removes it, Ctrl+Z undoes.");
		ImGui::PopStyleColor();
	}

	// ---------------------------------------------------------------------------------------------
	// Rules panel

	bool CanScore(const std::string& type)
	{
		return type == "lane" || type == "target" || type == "bumper" || type == "slingshot" || type == "post" ||
			type == "drop" || type == "kickout" || type == "spinner" || type == "ramp";
	}

	int PartById(const std::string& id)
	{
		auto& items = Parts().Items();
		for (size_t i = 0; i < items.size(); i++)
			if (items[i]["id"].Str() == id)
				return static_cast<int>(i);
		return -1;
	}

	// Ids of parts that can go into a group (lights == true: the lights).
	std::vector<std::string> Choices(bool lights)
	{
		std::vector<std::string> ids;
		for (auto& part : Parts().Items())
		{
			auto type = part["type"].Str();
			if ((lights ? type == "light" : CanScore(type)) && !part["id"].Str().empty())
				ids.push_back(part["id"].Str());
		}
		return ids;
	}

	bool Contains(const Value& list, const std::string& id)
	{
		for (auto& item : list.Items())
			if (item.Str() == id)
				return true;
		return false;
	}

	// Adds a part to a group, or takes it out when it is already in.
	void ToggleInGroup(Value& group, const std::string& id, bool light)
	{
		auto& list = group.At(light ? "lights" : "parts");
		if (!list.IsArray())
			list = Value::MakeArray();
		auto& items = list.MutableItems();
		for (size_t i = 0; i < items.size(); i++)
		{
			if (items[i].Str() == id)
			{
				items.erase(items.begin() + i);
				return;
			}
		}
		items.push_back(Value(id));
	}

	// A light picker: one id or none.
	void LightCombo(const char* label, Value& slot)
	{
		auto current = slot.Str();
		ImGui::SetNextItemWidth(-90);
		if (ImGui::BeginCombo(label, current.empty() ? "(none)" : current.c_str()))
		{
			if (ImGui::Selectable("(none)", current.empty()))
				slot = Value("");
			for (auto& id : Choices(true))
				if (ImGui::Selectable(id.c_str(), id == current))
					slot = Value(id);
			ImGui::EndCombo();
		}
	}

	void PartsList(Value& group, bool lights)
	{
		auto& list = group.At(lights ? "lights" : "parts");
		if (!list.IsArray())
			list = Value::MakeArray();
		auto& items = list.MutableItems();
		for (size_t i = 0; i < items.size(); i++)
		{
			ImGui::PushID(static_cast<int>(i));
			if (ImGui::SmallButton("x"))
			{
				items.erase(items.begin() + i);
				ImGui::PopID();
				break;
			}
			ImGui::SameLine();
			auto id = items[i].Str();
			ImGui::TextColored(PartById(id) < 0 ? ImVec4(1, 0.45f, 0.4f, 1) : ImVec4(0.9f, 0.9f, 0.95f, 1), "%s",
			                   id.c_str());
			ImGui::PopID();
		}
		ImGui::SetNextItemWidth(-1);
		if (ImGui::BeginCombo(lights ? "##addlight" : "##addpart", lights ? "Add a light..." : "Add a part..."))
		{
			for (auto& id : Choices(lights))
				if (!Contains(list, id) && ImGui::Selectable(id.c_str()))
					items.push_back(Value(id));
			ImGui::EndCombo();
		}
	}

	void RulesSettings()
	{
		auto& rules = Doc.At("rules");
		if (!rules.IsObject())
			rules = Value::MakeObject();
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::TextWrapped("Groups of parts give points and awards when they are all hit, or hit a number of "
		                   "times. Each part's light comes on as it is hit.");
		ImGui::PopStyleColor();
		ImGui::Spacing();

		ImGui::TextColored(ImVec4(0.75f, 0.65f, 1.0f, 1.0f), "Whole table");
		ImGui::Separator();
		EditFloat(rules, "ballSave", "Ball save", 0, 0, 30, 0.1f);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Seconds after a ball starts in which a drained ball is given back (once).\n0 = off.");
		LightCombo("Shoot again", rules.At("extraBallLight"));
		if (rules["extraBallLight"].Str().empty())
			rules.Remove("extraBallLight");
		auto& mult = rules.At("multiplierLights");
		if (!mult.IsArray())
			mult = Value::MakeArray();
		const char* multLabels[] = {"2X light", "3X light", "5X light", "10X light"};
		for (size_t i = 0; i < 4; i++)
		{
			auto& items = mult.MutableItems();
			Value slot = i < items.size() ? items[i] : Value("");
			auto before = slot.Str();
			ImGui::PushID(static_cast<int>(i));
			LightCombo(multLabels[i], slot);
			ImGui::PopID();
			if (slot.Str() != before)
			{
				while (items.size() <= i)
					items.push_back(Value(""));
				items[i] = slot;
			}
		}
		// Unused slots at the end are dropped; the game skips none.
		{
			auto& items = mult.MutableItems();
			while (!items.empty() && items.back().Str().empty())
				items.pop_back();
		}
		ImGui::Spacing();

		ImGui::TextColored(ImVec4(0.75f, 0.65f, 1.0f, 1.0f), "Groups");
		ImGui::Separator();
		auto& groupsValue = rules.At("groups");
		if (!groupsValue.IsArray())
			groupsValue = Value::MakeArray();
		auto& groups = groupsValue.MutableItems();
		HighlightGroup = PickGroup;
		for (size_t g = 0; g < groups.size(); g++)
		{
			auto& group = groups[g];
			ImGui::PushID(static_cast<int>(g));
			auto title = group["name"].Str("GROUP") + "###group";
			if (OpenGroup == static_cast<int>(g))
			{
				ImGui::SetNextItemOpen(true);
				OpenGroup = -1;
			}
			auto open = ImGui::CollapsingHeader(title.c_str());
			if (ImGui::IsItemHovered() && HighlightGroup < 0)
				HighlightGroup = static_cast<int>(g);
			if (open)
			{
				auto oldName = group["name"].Str("GROUP");
				if (EditText(group, "name", "Name"))
				{
					// Groups that start this mission follow the new name.
					auto newName = group["name"].Str("GROUP");
					for (auto& other : groups)
						if (other["startsMission"].Str() == oldName)
							other.At("startsMission") = Value(newName);
				}
				auto mission = group["mission"].Bool(false);
				if (ImGui::Checkbox("Mission (started by another group)", &mission))
				{
					if (mission)
						group.At("mission") = Value(true);
					else
					{
						group.Remove("mission");
						group.Remove("time");
						group.Remove("hint");
					}
				}
				if (mission)
				{
					EditFloat(group, "time", "Time limit", 0, 0, 300, 0.2f);
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("Seconds to finish it once started. 0 = until the ball drains.");
					EditText(group, "hint", "Hint");
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("Shown when the mission starts, like SHOOT THE HOLE.");
				}
				auto counting = group["hits"].Int(0) > 0;
				int mode = counting ? 1 : 0;
				const char* modes[] = {"Hit each one", "Count hits"};
				ImGui::SetNextItemWidth(-90);
				if (ImGui::Combo("Goal", &mode, modes, 2))
				{
					if (mode == 1)
						group.At("hits") = Value(10);
					else
						group.Remove("hits");
				}
				if (mode == 1)
					EditInt(group, "hits", "Hits", 10, 1, 999, 0.2f);

				ImGui::TextDisabled("Parts");
				PartsList(group, false);
				ImGui::TextDisabled(mode == 1 ? "Lights (fill up as hits add up)" : "Lights (one per part, in order)");
				PartsList(group, true);
				auto picking = PickGroup == static_cast<int>(g);
				if (picking)
					ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.55f, 0.3f, 1));
				if (ImGui::Button(picking ? "Done picking" : "Pick on the table", ImVec2(-1, 0)))
				{
					PickGroup = picking ? -1 : static_cast<int>(g);
					Selected = SelectedPoint = -1;
				}
				if (picking)
					ImGui::PopStyleColor();

				EditInt(group, "score", "Score", 5000, 0, 10000000, 50);
				EditInt(group, "bonus", "Bonus", 0, 0, 10000000, 50);
				{
					const char* keys[] = {"none", "multiplier", "extraBall", "multiball", "startMission"};
					const char* names[] = {"None", "Score multiplier up", "Extra ball", "2-ball multiball",
					                       "Start a mission"};
					auto award = group["award"].Str("none");
					int current = 0;
					for (int i = 0; i < 5; i++)
						if (award == keys[i])
							current = i;
					ImGui::SetNextItemWidth(-90);
					if (ImGui::Combo("Award", &current, names, 5))
					{
						if (current == 0)
							group.Remove("award");
						else
							group.At("award") = Value(keys[current]);
						if (current != 4)
							group.Remove("startsMission");
					}
					if (current == 4)
					{
						auto target = group["startsMission"].Str();
						ImGui::SetNextItemWidth(-90);
						if (ImGui::BeginCombo("Mission", target.empty() ? "(pick one)" : target.c_str()))
						{
							bool any = false;
							for (size_t m = 0; m < groups.size(); m++)
							{
								if (m == g || !groups[m]["mission"].Bool(false))
									continue;
								any = true;
								auto name = groups[m]["name"].Str("GROUP");
								if (ImGui::Selectable(name.c_str(), name == target))
									group.At("startsMission") = Value(name);
							}
							if (!any)
								ImGui::TextDisabled("Tick \"Mission\" on another group first");
							ImGui::EndCombo();
						}
					}
				}
				EditText(group, "message", "Message");
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Shown when the group is done. Empty: \"NAME COMPLETE\".");
				if (ImGui::SmallButton("Delete group"))
				{
					groups.erase(groups.begin() + g);
					PickGroup = HighlightGroup = -1;
					ImGui::PopID();
					break;
				}
				ImGui::Spacing();
			}
			ImGui::PopID();
		}
		if (ImGui::Button("Add a group", ImVec2(-1, 0)))
		{
			auto group = Value::MakeObject();
			group.Set("name", Value("GROUP " + std::to_string(groups.size() + 1)));
			group.Set("parts", Value::MakeArray());
			group.Set("lights", Value::MakeArray());
			group.Set("score", Value(5000));
			groups.push_back(group);
			PickGroup = OpenGroup = static_cast<int>(groups.size()) - 1;
			Selected = SelectedPoint = -1;
		}
	}

	// Rules group for the table view's outlines, or nullptr.
	const Value* GroupAt(int index)
	{
		auto& groups = Doc["rules"]["groups"].Items();
		return index >= 0 && index < static_cast<int>(groups.size()) ? &groups[index] : nullptr;
	}

	// ---------------------------------------------------------------------------------------------

	void AddPart(const std::string& type)
	{
		auto centre = ToTable(ImVec2(CanvasCenter.x, CanvasCenter.y));
		auto x = SnapValue(centre.x), y = SnapValue(centre.y);
		auto part = Value::MakeObject();
		part.Set("type", Value(type));
		part.Set("id", Value(UniqueId(type)));
		auto pts = Value::MakeArray();
		if (type == "wall")
		{
			pts.Push(Point(x - 1, y));
			pts.Push(Point(x + 1, y));
		}
		else if (type == "slingshot")
		{
			pts.Push(Point(x - 0.8f, y - 1.2f));
			pts.Push(Point(x - 0.8f, y + 1.2f));
			pts.Push(Point(x + 0.8f, y + 1.8f));
		}
		else if (type == "lane" || type == "gate")
		{
			pts.Push(Point(x - 0.8f, y));
			pts.Push(Point(x + 0.8f, y));
		}
		else if (type == "ramp")
		{
			pts.Push(Point(x, y + 2.5f));
			pts.Push(Point(x, y));
			pts.Push(Point(x + 1.5f, y - 2.0f));
		}
		if (pts.Size())
			part.Set("points", pts);
		else
		{
			part.Set("x", Value(x));
			part.Set("y", Value(y));
		}
		if (type == "flipper")
		{
			part.Set("side", Value(CountType("flipper", "left") ? "right" : "left"));
			part.Set("length", Value(2.2f));
		}
		Parts().Push(part);
		Selected = static_cast<int>(Parts().Size()) - 1;
		SelectedPoint = -1;
	}

	void DeleteSelected()
	{
		auto& items = Parts().MutableItems();
		if (Selected < 0 || Selected >= static_cast<int>(items.size()))
			return;
		auto id = items[Selected]["id"].Str();
		items.erase(items.begin() + Selected);
		if (!id.empty() && !IdUsed(id, -1))
			RemoveRefs(id);
		Selected = -1;
		SelectedPoint = -1;
	}

	void DuplicateSelected()
	{
		auto& items = Parts().MutableItems();
		if (Selected < 0 || Selected >= static_cast<int>(items.size()))
			return;
		auto copy = items[Selected];
		auto type = copy["type"].Str();
		if (type == "flipper" || type == "plunger" || type == "drain")
		{
			SetStatus("A table has just one of those");
			return;
		}
		copy.Set("id", Value(UniqueId(type)));
		auto original = copy;
		MovePart(copy, original, 1.0f, 1.0f);
		items.push_back(copy);
		Selected = static_cast<int>(items.size()) - 1;
	}

	std::string Problems()
	{
		if (!CountType("flipper", "left") || !CountType("flipper", "right"))
			return "Needs a left and a right flipper";
		if (CountType("flipper", "left") > 1 || CountType("flipper", "right") > 1)
			return "Only one left and one right flipper for now";
		if (CountType("plunger") != 1)
			return "Needs one plunger";
		return "";
	}

	void Canvas()
	{
		auto draw = ImGui::GetWindowDrawList();
		auto origin = ImGui::GetCursorScreenPos();
		auto size = ImGui::GetContentRegionAvail();
		if (size.x < 50 || size.y < 50)
			return;
		ImGui::InvisibleButton("canvas", size,
		                       ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
		                       ImGuiButtonFlags_MouseButtonMiddle);
		auto hovered = ImGui::IsItemHovered();
		auto& io = ImGui::GetIO();

		auto w = TableW(), l = TableL();
		CanvasCenter = ImVec2(origin.x + size.x / 2, origin.y + size.y / 2);
		CanvasScale = std::min(size.x / (w + 3), size.y / (l + 3)) * Zoom;

		// Zoom with the wheel around the mouse.
		if (hovered && io.MouseWheel != 0)
		{
			auto before = ToTable(io.MousePos);
			Zoom = std::max(0.5f, std::min(6.0f, Zoom * std::pow(1.15f, io.MouseWheel)));
			CanvasScale = std::min(size.x / (w + 3), size.y / (l + 3)) * Zoom;
			auto after = ToScreen(before.x, before.y);
			Pan.x += io.MousePos.x - after.x;
			Pan.y += io.MousePos.y - after.y;
		}

		TablePreview preview;
		customtable::BuildPreview(Doc, preview);
		auto& parts = Parts().MutableItems();
		auto mouse = ToTable(io.MousePos);

		// Mouse.
		if (hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle)))
		{
			Dragging = DragMode::View;
			DragStart = io.MousePos;
		}
		if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && PickGroup >= 0)
		{
			auto picked = PickPart(preview, mouse);
			auto& groups = Doc.At("rules").At("groups").MutableItems();
			if (picked >= 0 && PickGroup < static_cast<int>(groups.size()))
			{
				auto type = parts[picked]["type"].Str();
				auto id = parts[picked]["id"].Str();
				if (id.empty())
				{
					id = UniqueId(type);
					parts[picked].At("id") = Value(id);
				}
				if (type == "light" || CanScore(type))
					ToggleInGroup(groups[PickGroup], id, type == "light");
				else
					SetStatus("That part can't be in a group");
			}
		}
		else if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			Dragging = DragMode::None;
			// A point of the selected part first.
			if (Selected >= 0 && Selected < static_cast<int>(parts.size()))
			{
				auto handles = Handles(parts[Selected]);
				for (size_t i = 0; i < handles.size(); i++)
				{
					auto p = ToScreen(handles[i].x, handles[i].y);
					if (std::fabs(p.x - io.MousePos.x) <= 7 && std::fabs(p.y - io.MousePos.y) <= 7)
					{
						SelectedPoint = static_cast<int>(i);
						Dragging = DragMode::Point;
						break;
					}
				}
			}
			if (Dragging == DragMode::None)
			{
				auto picked = PickPart(preview, mouse);
				if (picked != Selected)
					SelectedPoint = -1;
				Selected = picked;
				if (Selected >= 0)
					Dragging = DragMode::Body;
			}
			if (Dragging != DragMode::None)
			{
				DragStart = mouse;
				DragOriginal = parts[Selected];
			}
		}
		if (Dragging != DragMode::None && !ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
			!ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle))
			Dragging = DragMode::None;

		if (Dragging == DragMode::View)
		{
			Pan.x += io.MouseDelta.x;
			Pan.y += io.MouseDelta.y;
		}
		else if (Dragging == DragMode::Body && Selected >= 0 && Selected < static_cast<int>(parts.size()))
		{
			MovePart(parts[Selected], DragOriginal, mouse.x - DragStart.x, mouse.y - DragStart.y);
		}
		else if (Dragging == DragMode::Point && Selected >= 0 && Selected < static_cast<int>(parts.size()))
		{
			auto& pts = parts[Selected].At("points").MutableItems();
			if (SelectedPoint >= 0 && SelectedPoint < static_cast<int>(pts.size()))
				pts[SelectedPoint] = Point(SnapValue(mouse.x), SnapValue(mouse.y));
		}

		// Drawing.
		draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
		draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(12, 12, 18, 255));
		auto p0 = ToScreen(-w / 2, -l / 2), p1 = ToScreen(w / 2, l / 2);
		if (auto picture = Picture())
			draw->AddImage(picture, p0, p1);
		else
			draw->AddRectFilledMultiColor(p0, p1, preview.Playfield, preview.Playfield, preview.Playfield2,
			                              preview.Playfield2);
		for (float gx = std::ceil(-w / 2); gx <= w / 2; gx += 1)
			draw->AddLine(ToScreen(gx, -l / 2), ToScreen(gx, l / 2),
			              gx == 0 ? IM_COL32(255, 255, 255, 50) : IM_COL32(255, 255, 255, 18));
		for (float gy = std::ceil(-l / 2); gy <= l / 2; gy += 1)
			draw->AddLine(ToScreen(-w / 2, gy), ToScreen(w / 2, gy),
			              gy == 0 ? IM_COL32(255, 255, 255, 50) : IM_COL32(255, 255, 255, 18));
		draw->AddRect(p0, p1, IM_COL32(255, 255, 255, 90));

		auto hoverPart = hovered && Dragging == DragMode::None ? PickPart(preview, mouse) : -1;
		for (auto& shape : preview.Shapes)
		{
			if (shape.PartIndex == Selected)
				DrawShape(draw, shape, IM_COL32(255, 220, 60, 255), 3);
			else if (shape.PartIndex == hoverPart)
				DrawShape(draw, shape, IM_COL32(255, 255, 255, 120), 2);
		}
		if (auto group = GroupAt(HighlightGroup))
		{
			for (auto& shape : preview.Shapes)
			{
				if (shape.PartIndex < 0 || shape.PartIndex >= static_cast<int>(parts.size()))
					continue;
				auto id = parts[shape.PartIndex]["id"].Str();
				if (Contains((*group)["parts"], id))
					DrawShape(draw, shape, IM_COL32(80, 255, 120, 255), 3);
				else if (Contains((*group)["lights"], id))
					DrawShape(draw, shape, IM_COL32(255, 230, 80, 255), 3);
			}
		}
		for (auto& shape : preview.Shapes)
			DrawShape(draw, shape, shape.Color, 0);

		// Flipper swing, faint.
		for (auto& part : parts)
		{
			if (part["type"].Str() != "flipper")
				continue;
			auto left = part["side"].Str("left") != "right";
			auto x = part["x"].Float(), y = part["y"].Float(), len = part["length"].Float(2.0f);
			auto a = (part["angle"].Float(30.0f) - part["swing"].Float(55.0f)) * Pi / 180.0f;
			draw->AddLine(ToScreen(x, y), ToScreen(x + (left ? 1 : -1) * std::cos(a) * len, y + std::sin(a) * len),
			              IM_COL32(255, 255, 255, 60), std::max(1.5f, 0.4f * CanvasScale));
		}

		if (Selected >= 0 && Selected < static_cast<int>(parts.size()))
		{
			auto handles = Handles(parts[Selected]);
			for (size_t i = 0; i < handles.size(); i++)
			{
				auto p = ToScreen(handles[i].x, handles[i].y);
				auto on = static_cast<int>(i) == SelectedPoint;
				draw->AddRectFilled(ImVec2(p.x - 5, p.y - 5), ImVec2(p.x + 5, p.y + 5),
				                    on ? IM_COL32(255, 220, 60, 255) : IM_COL32(255, 255, 255, 230));
				draw->AddRect(ImVec2(p.x - 5, p.y - 5), ImVec2(p.x + 5, p.y + 5), IM_COL32(0, 0, 0, 255));
			}
		}
		if (hovered)
		{
			char pos[48];
			snprintf(pos, sizeof pos, "%.1f, %.1f", mouse.x, mouse.y);
			draw->AddText(ImVec2(origin.x + 8, origin.y + size.y - 20), IM_COL32(200, 200, 210, 200), pos);
		}
		if (auto group = GroupAt(PickGroup))
		{
			auto text = "Click parts and lights to add them to " + (*group)["name"].Str("the group") +
				" (click again to take them out).";
			draw->AddRectFilled(ImVec2(origin.x, origin.y), ImVec2(origin.x + size.x, origin.y + 26),
			                    IM_COL32(30, 90, 50, 230));
			draw->AddText(ImVec2(origin.x + 8, origin.y + 6), IM_COL32(230, 255, 235, 255), text.c_str());
		}
		else
			draw->AddText(ImVec2(origin.x + 8, origin.y + 6), IM_COL32(170, 170, 190, 200),
			              "Drag parts to move them. Right-drag to pan, wheel to zoom.");
		draw->PopClipRect();
	}
}

bool tableeditor::Edit(const std::string& path)
{
	Value doc;
	bool packed = false;
	std::string error;
	if (!customtable::ReadDocument(path, doc, packed, error))
	{
		pb::ShowMessageBox(SDL_MESSAGEBOX_WARNING, "Edit Table", error.c_str());
		return false;
	}
	if (packed)
	{
		pb::ShowMessageBox(SDL_MESSAGEBOX_WARNING, "Edit Table",
		                   "This table is packed with its pictures. Editing packed tables isn't supported yet.");
		return false;
	}
	Doc = doc;
	// The editor writes into these, so make sure they exist before the first undo snapshot.
	for (auto key : {"playfield", "theme", "rules"})
		if (!Doc[key].IsObject())
			Doc.Set(key, Value::MakeObject());
	if (!Doc["parts"].IsArray())
		Doc.Set("parts", Value::MakeArray());
	auto& rules = Doc.At("rules");
	for (auto key : {"groups", "multiplierLights"})
		if (!rules[key].IsArray())
			rules.Set(key, Value::MakeArray());
	for (auto& group : rules.At("groups").MutableItems())
		for (auto key : {"parts", "lights"})
			if (!group[key].IsArray())
				group.Set(key, Value::MakeArray());
	FilePath = path;
	Dirty = false;
	Selected = SelectedPoint = -1;
	UndoStack.clear();
	RedoStack.clear();
	Pending = false;
	Zoom = 1;
	Pan = ImVec2(0, 0);
	Status.clear();
	FreePicture();
	PickGroup = HighlightGroup = -1;
	if (!Open)
		winmain::pause(false);
	Open = true;
	return true;
}

void tableeditor::NewTable()
{
	// In the folder of the table being played, else the first Tables folder found, else next to the game.
	std::string folder;
	if (customtable::Active)
	{
		auto cut = customtable::Path.find_last_of("/\\");
		if (cut != std::string::npos)
			folder = customtable::Path.substr(0, cut);
	}
	for (size_t i = 0; folder.empty() && i < customtable::SearchFolders.size(); i++)
		if (!customtable::FindTables({customtable::SearchFolders[i]}).empty())
			folder = customtable::SearchFolders[i] + "Tables";
	if (folder.empty())
		folder = (customtable::SearchFolders.size() > 1 ? customtable::SearchFolders[1] : std::string()) + "Tables";
	MakeFolder(folder);

	std::string name, path;
	for (int n = 1;; n++)
	{
		name = n == 1 ? "My Table" : "My Table " + std::to_string(n);
		path = folder + PathSeparator + name + ".pwtable";
		if (!FileExists(path))
			break;
	}
	auto doc = StarterTable(name);
	if (!WriteFile(path, doc.Dump(2) + "\n"))
	{
		pb::ShowMessageBox(SDL_MESSAGEBOX_WARNING, "New Table", ("Couldn't create " + path).c_str());
		return;
	}
	Edit(path);
	SetStatus("New table made from the starter layout");
}

void tableeditor::Render()
{
	if (!Open)
		return;

	auto docBefore = Doc.Dump();
	if (!Pending)
		PendingBase = docBefore;

	auto viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->WorkPos);
	ImGui::SetNextWindowSize(viewport->WorkSize);
	ImGui::SetNextWindowBgAlpha(1.0f);
	ImGui::Begin("Table Editor", nullptr,
	             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
	             ImGuiWindowFlags_NoScrollWithMouse);

	// Toolbar.
	auto name = Doc["name"].Str("Untitled");
	ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.4f, 1.0f), "Editing: %s%s", name.c_str(), Dirty ? " *" : "");
	ImGui::SameLine();
	if (ImGui::Button("Save"))
		Save();
	ImGui::SameLine();
	auto problems = Problems();
	ImGui::BeginDisabled(!problems.empty());
	if (ImGui::Button("Save and Play"))
	{
		if (Save())
		{
			options::Options.TableFile.V = FilePath;
			Close();
			winmain::Restart();
		}
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(UndoStack.empty());
	if (ImGui::Button("Undo"))
		Undo();
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(RedoStack.empty());
	if (ImGui::Button("Redo"))
		Redo();
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::Checkbox("Snap to grid", &Snap);
	ImGui::SameLine();
	if (ImGui::Button("Fit"))
	{
		Zoom = 1;
		Pan = ImVec2(0, 0);
	}
	ImGui::SameLine();
	if (ImGui::Button("Close"))
	{
		if (Dirty)
			AskClose = true;
		else
			Close();
	}
	if (!problems.empty())
	{
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", problems.c_str());
	}
	else if (!Status.empty() && ImGui::GetTime() < StatusUntil)
	{
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.6f, 1.0f), "%s", Status.c_str());
	}
	ImGui::Separator();

	// Parts to add.
	ImGui::BeginChild("parts", ImVec2(130, 0), true);
	ImGui::TextColored(ImVec4(0.75f, 0.65f, 1.0f, 1.0f), "Add");
	ImGui::Separator();
	const char* types[] = {"wall", "post", "bumper", "slingshot", "target", "drop", "lane", "light", "kickout",
	                       "spinner", "ramp", "gate"};
	const char* labels[] = {"Wall", "Post", "Bumper", "Slingshot", "Target", "Drop target", "Lane switch", "Light",
	                        "Kickout hole", "Spinner", "Ramp", "One-way gate"};
	for (int i = 0; i < 12; i++)
	{
		if (ImGui::Button(labels[i], ImVec2(-1, 0)))
			AddPart(types[i]);
	}
	if (CountType("flipper") < 2 && ImGui::Button("Flipper", ImVec2(-1, 0)))
		AddPart("flipper");
	if (CountType("plunger") < 1 && ImGui::Button("Plunger", ImVec2(-1, 0)))
		AddPart("plunger");
	ImGui::Spacing();
	auto hasSelection = Selected >= 0 && Selected < static_cast<int>(Parts().Size());
	ImGui::BeginDisabled(!hasSelection);
	ImGui::TextColored(ImVec4(0.75f, 0.65f, 1.0f, 1.0f), "Selected");
	ImGui::Separator();
	if (ImGui::Button("Copy", ImVec2(-1, 0)))
		DuplicateSelected();
	if (ImGui::Button("Delete", ImVec2(-1, 0)))
		DeleteSelected();
	ImGui::EndDisabled();
	ImGui::EndChild();
	ImGui::SameLine();

	// Table view.
	auto settingsW = 300.0f;
	ImGui::BeginChild("view", ImVec2(ImGui::GetContentRegionAvail().x - settingsW - 8, 0), true,
	                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	Canvas();
	ImGui::EndChild();
	ImGui::SameLine();

	// Settings.
	ImGui::BeginChild("settings", ImVec2(0, 0), true);
	RulesTab = false;
	if (ImGui::BeginTabBar("side"))
	{
		auto hasPart = Selected >= 0 && Selected < static_cast<int>(Parts().Size());
		if (ImGui::BeginTabItem(hasPart ? "Part###settings" : "Table###settings"))
		{
			if (hasPart)
				PartSettings(Parts().MutableItems()[Selected]);
			else
				TableSettings();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Rules"))
		{
			RulesTab = true;
			RulesSettings();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	if (!RulesTab)
		PickGroup = HighlightGroup = -1;
	ImGui::EndChild();

	// Keys.
	auto& io = ImGui::GetIO();
	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput)
	{
		if (ImGui::IsKeyPressed(ImGuiKey_Delete) && Selected >= 0)
			DeleteSelected();
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z))
			Undo();
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y))
			Redo();
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S))
			Save();
		if (ImGui::IsKeyPressed(ImGuiKey_Escape))
		{
			PickGroup = -1;
			Selected = -1;
			SelectedPoint = -1;
		}
	}

	// Leaving with unsaved changes.
	if (AskClose)
		ImGui::OpenPopup("Save changes?");
	if (ImGui::BeginPopupModal("Save changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("Save your changes to this table?");
		if (ImGui::Button("Save", ImVec2(100, 0)))
		{
			AskClose = false;
			ImGui::CloseCurrentPopup();
			if (Save())
				Close();
		}
		ImGui::SameLine();
		if (ImGui::Button("Don't save", ImVec2(100, 0)))
		{
			AskClose = false;
			ImGui::CloseCurrentPopup();
			Close();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(100, 0)))
		{
			AskClose = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
	ImGui::End();

	// Undo: one step per finished change (a whole drag, a whole slider move).
	if (SkipChange)
	{
		SkipChange = false;
	}
	else if (Open && Doc.Dump() != docBefore)
	{
		Dirty = true;
		Pending = true;
		RedoStack.clear();
	}
	if (Pending && Dragging == DragMode::None && !ImGui::IsAnyItemActive())
	{
		UndoStack.push_back(PendingBase);
		if (UndoStack.size() > 100)
			UndoStack.erase(UndoStack.begin());
		Pending = false;
	}
}
