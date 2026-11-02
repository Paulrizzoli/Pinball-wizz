#include "pch.h"
#include "tablepicker.h"

#include "customtable.h"
#include "high_score.h"
#include "options.h"
#include "pb.h"
#include "winmain.h"

#include <cmath>

bool tablepicker::Open = false;

namespace
{
	std::vector<CustomTableEntry> Tables;

	std::string FormatScore(int score)
	{
		auto digits = std::to_string(score);
		std::string out;
		for (size_t i = 0; i < digits.size(); i++)
		{
			if (i && (digits.size() - i) % 3 == 0)
				out += ',';
			out += digits[i];
		}
		return out;
	}

	void DrawPreview(ImDrawList* draw, const TablePreview& preview, ImVec2 boxMin, ImVec2 boxMax)
	{
		auto boxW = boxMax.x - boxMin.x, boxH = boxMax.y - boxMin.y;
		auto s = std::min(boxW / preview.Width, boxH / preview.Length);
		ImVec2 centre{(boxMin.x + boxMax.x) * 0.5f, (boxMin.y + boxMax.y) * 0.5f};
		auto map = [&](float x, float y) { return ImVec2(centre.x + x * s, centre.y + y * s); };

		auto p0 = map(-preview.Width / 2, -preview.Length / 2), p1 = map(preview.Width / 2, preview.Length / 2);
		draw->AddRectFilledMultiColor(p0, p1, preview.Playfield, preview.Playfield, preview.Playfield2,
		                              preview.Playfield2);
		draw->PushClipRect(p0, p1, true);
		for (auto& shape : preview.Shapes)
		{
			std::vector<ImVec2> pts;
			for (size_t i = 0; i + 1 < shape.Points.size(); i += 2)
				pts.push_back(map(shape.Points[i], shape.Points[i + 1]));
			if (pts.empty())
				continue;
			switch (shape.Kind)
			{
			case TablePreviewShape::Circle:
				draw->AddCircleFilled(pts[0], std::max(1.5f, shape.Radius * s), shape.Color, 16);
				break;
			case TablePreviewShape::Line:
			case TablePreviewShape::Polygon:
				if (shape.Filled && pts.size() >= 3)
				{
					draw->AddConvexPolyFilled(pts.data(), static_cast<int>(pts.size()), shape.Color);
				}
				else
				{
					auto width = std::max(1.5f, shape.Width * s);
					draw->AddPolyline(pts.data(), static_cast<int>(pts.size()), shape.Color, 0, width);
					for (auto& p : pts)
						draw->AddCircleFilled(p, width * 0.5f, shape.Color, 8);
				}
				break;
			}
		}
		draw->PopClipRect();
	}

	// One card. Returns true when clicked.
	bool Card(const char* id, const std::string& name, const std::string& author, int best, bool current,
	          const TablePreview* preview)
	{
		const ImVec2 size{176, 330};
		const float previewH = 236;
		auto draw = ImGui::GetWindowDrawList();
		auto pos = ImGui::GetCursorScreenPos();
		ImGui::PushID(id);
		auto clicked = ImGui::InvisibleButton("card", size);
		auto hovered = ImGui::IsItemHovered();
		ImGui::PopID();

		ImVec2 max{pos.x + size.x, pos.y + size.y};
		auto accent = preview ? preview->Accent : IM_COL32(150, 120, 255, 255);
		draw->AddRectFilled(pos, max, hovered ? IM_COL32(44, 44, 64, 255) : IM_COL32(28, 28, 42, 255), 8);
		draw->AddRect(pos, max, current || hovered ? accent : IM_COL32(70, 70, 95, 255), 8, 0,
		              current ? 2.5f : 1.0f);

		ImVec2 boxMin{pos.x + 10, pos.y + 10}, boxMax{max.x - 10, pos.y + 10 + previewH};
		if (preview)
		{
			DrawPreview(draw, *preview, boxMin, boxMax);
		}
		else
		{
			// Space Cadet: no picture, just its name on a starry panel.
			draw->AddRectFilledMultiColor(boxMin, boxMax, IM_COL32(20, 16, 50, 255), IM_COL32(20, 16, 50, 255),
			                              IM_COL32(5, 5, 15, 255), IM_COL32(5, 5, 15, 255));
			for (int i = 0; i < 40; i++)
			{
				auto x = boxMin.x + std::fmod(i * 37.7f, boxMax.x - boxMin.x);
				auto y = boxMin.y + std::fmod(i * 53.3f, boxMax.y - boxMin.y);
				draw->AddCircleFilled(ImVec2(x, y), (i % 3) * 0.5f + 0.6f, IM_COL32(220, 220, 255, 160));
			}
			auto text = "3D PINBALL";
			auto ts = ImGui::CalcTextSize(text);
			draw->AddText(ImVec2((boxMin.x + boxMax.x - ts.x) * 0.5f, (boxMin.y + boxMax.y) * 0.5f - ts.y),
			              IM_COL32(200, 180, 255, 255), text);
		}

		auto textY = boxMax.y + 10;
		auto line = [&](const std::string& text, ImU32 color)
		{
			auto clipped = text;
			while (clipped.size() > 3 && ImGui::CalcTextSize(clipped.c_str()).x > size.x - 20)
				clipped = clipped.substr(0, clipped.size() - 4) + "...";
			draw->AddText(ImVec2(pos.x + 10, textY), color, clipped.c_str());
			textY += ImGui::GetTextLineHeightWithSpacing();
		};
		line(name, IM_COL32(255, 255, 255, 255));
		line(author.empty() ? std::string(" ") : "by " + author, IM_COL32(170, 170, 190, 255));
		line(best > 0 ? "Best: " + FormatScore(best) : "No high score yet", IM_COL32(255, 200, 120, 255));
		if (current)
			line("Playing now", accent);

		if (hovered)
			ImGui::SetTooltip(current ? "You are playing this table" : "Play %s", name.c_str());
		return clicked && !current;
	}
}

void tablepicker::Show()
{
	Tables = customtable::FindTables(customtable::SearchFolders);
	if (!Open)
	{
		Open = true;
		winmain::pause(false);
	}
}

void tablepicker::FirstStart()
{
	if (options::get_int("Table Picker Shown", 0))
		return;
	options::set_int("Table Picker Shown", 1);
	if (!customtable::FindTables(customtable::SearchFolders).empty())
		Show();
}

void tablepicker::Render()
{
	if (!Open)
		return;

	auto viewport = ImGui::GetMainViewport();
	// Kept centred and inside the game window, which can change size (full screen, resizing).
	ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(std::min(820.0f, viewport->Size.x * 0.96f),
	                                std::min(560.0f, viewport->Size.y * 0.9f)), ImGuiCond_Always);
	if (ImGui::Begin("Choose a Table", &Open,
	                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize))
	{
		ImGui::TextColored(ImVec4(0.75f, 0.65f, 1.0f, 1.0f), "Pick a table to play");
		ImGui::Spacing();

		auto cardW = 176.0f, gap = ImGui::GetStyle().ItemSpacing.x;
		auto perRow = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + gap) / (cardW + gap)));
		int index = 0;
		std::string pick;
		bool pickSpaceCadet = false;
		auto next = [&]()
		{
			if (++index % perRow != 0)
				ImGui::SameLine();
		};

		if (!pb::DatFileName.empty())
		{
			if (Card("space cadet", "Space Cadet", "", high_score::BestScore(false, ""), !customtable::Active,
			         nullptr))
				pickSpaceCadet = true;
			next();
		}
		for (auto& table : Tables)
		{
			auto current = customtable::Active && table.Path == customtable::Path;
			if (Card(table.Path.c_str(), table.Name, table.Author, high_score::BestScore(true, table.Name), current,
			         &table.Preview))
				pick = table.Path;
			next();
		}
		if (index % perRow != 0)
			ImGui::NewLine();

		ImGui::Spacing();
		ImGui::TextDisabled("More tables: put .pwtable files in the Tables folder next to the game.");
		ImGui::Spacing();
		if (ImGui::Button("Close", ImVec2(120, 0)))
			Open = false;

		if (pickSpaceCadet || !pick.empty())
		{
			options::Options.TableFile.V = pick;
			Open = false;
			winmain::Restart();
		}
	}
	ImGui::End();
	if (!Open)
		winmain::end_pause();
}
