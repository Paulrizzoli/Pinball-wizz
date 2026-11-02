#include "pch.h"
#include "modernfx.h"

#include "ledfx.h"
#include "options.h"
#include "pb.h"
#include "render.h"
#include "score.h"
#include "TPinballTable.h"
#include "winmain.h"

std::vector<modernfx::Particle> modernfx::Particles;
std::unordered_map<const void*, std::vector<float>> modernfx::Trails;
float modernfx::ShakeTime = 10.0f, modernfx::ShakeDirX = 0, modernfx::ShakeDirY = 0;
uint64_t modernfx::LastTicks = 0;
float modernfx::FrameDt = 0;
SDL_Texture *modernfx::Dot = nullptr, *modernfx::Ambient = nullptr, *modernfx::Scanlines = nullptr,
            *modernfx::Vignette = nullptr;
int modernfx::ScanlinesHeight = 0;
bool modernfx::SettingsOpen = false;

namespace
{
	constexpr float ShakeDuration = 0.35f;
	constexpr int TrailLength = 9;

	float Random(float lo, float hi)
	{
		return lo + (hi - lo) * (static_cast<float>(std::rand()) / RAND_MAX);
	}

	SDL_Texture* CreateTexture(const std::vector<ColorRgba>& pixels, int width, int height, SDL_BlendMode blend,
	                           bool linear)
	{
		auto texture = SDL_CreateTexture(winmain::Renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
		                                 width, height);
		if (!texture)
			return nullptr;
		SDL_UpdateTexture(texture, nullptr, pixels.data(), width * 4);
		SDL_SetTextureBlendMode(texture, blend);
#if SDL_VERSION_ATLEAST(2, 0, 12)
		SDL_SetTextureScaleMode(texture, linear ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
#endif
		return texture;
	}

	void BoardToScreen(float x, float y, float& sx, float& sy)
	{
		float originX, originY, scaleX, scaleY;
		SDL_Rect clip;
		render::GetBoardTransform(originX, originY, scaleX, scaleY, clip);
		sx = originX + x * scaleX;
		sy = originY + y * scaleY;
	}
}

SDL_Texture* modernfx::GetDot()
{
	if (!Dot)
	{
		constexpr int size = 32;
		std::vector<ColorRgba> pixels(size * size);
		for (int y = 0; y < size; y++)
			for (int x = 0; x < size; x++)
			{
				auto dx = (x + 0.5f) / size * 2 - 1, dy = (y + 0.5f) / size * 2 - 1;
				auto v = std::max(0.0f, 1.0f - std::sqrt(dx * dx + dy * dy));
				v = v * v * (3 - 2 * v);
				auto c = static_cast<uint8_t>(v * 255.0f);
				pixels[y * size + x] = ColorRgba{c, c, c, 255};
			}
		Dot = CreateTexture(pixels, size, size, SDL_BLENDMODE_ADD, true);
	}
	return Dot;
}

void modernfx::BumperHit(float x, float y, float radius, const uint8_t color[3])
{
	// One soft flash, then a burst of sparks.
	Particle flash{x, y, 0, 0, 0.18f, 0.18f, radius * 4.2f, {color[0], color[1], color[2]}};
	Particles.push_back(flash);
	for (int i = 0; i < 14; i++)
	{
		auto angle = Random(0, 6.2832f);
		auto speed = Random(70.0f, 170.0f);
		auto life = Random(0.22f, 0.5f);
		Particle spark{
			x + std::cos(angle) * radius * 0.6f, y + std::sin(angle) * radius * 0.6f,
			std::cos(angle) * speed, std::sin(angle) * speed, life, life, Random(2.2f, 3.6f),
			{
				static_cast<uint8_t>(std::min(255, color[0] + 90)), static_cast<uint8_t>(std::min(255, color[1] + 90)),
				static_cast<uint8_t>(std::min(255, color[2] + 90))
			}
		};
		Particles.push_back(spark);
	}
	if (Particles.size() > 400)
		Particles.erase(Particles.begin(), Particles.begin() + (Particles.size() - 400));
}

void modernfx::Nudge(float dx, float dy)
{
	if (!options::Options.NudgeShake)
		return;
	auto len = std::sqrt(dx * dx + dy * dy);
	ShakeDirX = len > 0 ? dx / len : 0;
	ShakeDirY = len > 0 ? dy / len : 1;
	ShakeTime = 0;
}

void modernfx::BeginFrame(int& shakeX, int& shakeY)
{
	auto now = SDL_GetTicks64();
	FrameDt = LastTicks ? std::min(0.1f, (now - LastTicks) / 1000.0f) : 0.0f;
	LastTicks = now;

	// Particles keep moving even while paused; they are short-lived.
	for (auto& p : Particles)
	{
		p.Life -= FrameDt;
		p.X += p.VX * FrameDt;
		p.Y += p.VY * FrameDt;
		p.VY += 160.0f * FrameDt;
		p.VX *= 1.0f - 2.5f * FrameDt;
		p.VY *= 1.0f - 2.5f * FrameDt;
	}
	Particles.erase(std::remove_if(Particles.begin(), Particles.end(), [](const Particle& p) { return p.Life <= 0; }),
	                Particles.end());

	shakeX = shakeY = 0;
	ShakeTime += FrameDt;
	if (options::Options.NudgeShake && ShakeTime < ShakeDuration && render::vscreen)
	{
		// Damped wobble along the nudge direction, about 5 table pixels at its peak.
		auto scale = static_cast<float>(render::DestinationRect.w) / render::vscreen->Width;
		auto t = ShakeTime;
		auto amount = 5.0f * scale * std::exp(-t / 0.09f) * std::cos(t * 2 * 3.14159f * 14.0f);
		shakeX = static_cast<int>(std::round(ShakeDirX * amount));
		shakeY = static_cast<int>(std::round(ShakeDirY * amount));
	}
}

bool modernfx::WideLayout(SDL_Rect& screenRect, SDL_Rect& left, SDL_Rect& right)
{
	auto screen = render::vscreen;
	if (!options::Options.Backglass || !screen || !pb::MainTable)
		return false;
	int width, height;
	if (SDL_GetRendererOutputSize(winmain::Renderer, &width, &height) != 0)
		return false;
	auto top = winmain::MainMenuHeight;
	auto areaH = height - top;
	if (areaH <= 0)
		return false;

	// Only when the game would otherwise get side bars, and the side panels get a useful size.
	if (width * screen->Height < areaH * screen->Width * 21 / 20)
		return false;
	auto scale = static_cast<float>(areaH) / screen->Height;
	auto boardW = static_cast<int>(pb::MainTable->Width * scale);
	auto sideW = (width - boardW) / 2;
	if (sideW < (screen->Width - pb::MainTable->Width) * scale * 0.6f)
		return false;

	screenRect = SDL_Rect{sideW, top, static_cast<int>(screen->Width * scale), areaH};
	left = SDL_Rect{0, top, sideW, areaH};
	right = SDL_Rect{sideW + boardW, top, width - sideW - boardW, areaH};
	return true;
}

void modernfx::PanelRegions(SDL_Rect& logo, SDL_Rect& scores)
{
	// Sidebar of the original layout, right of the table: the logo backglass on top (it holds the small
	// ball-number box), then the score row and message boxes. The first box below 40% of the height
	// starts the score panel.
	auto screen = render::vscreen;
	auto tableWidth = pb::MainTable ? pb::MainTable->Width : screen->Width * 6 / 10;
	auto sidebarWidth = screen->Width - tableWidth;
	auto split = screen->Height * 45 / 100;
	auto best = screen->Height;
	for (auto& rect : ledfx::DisplayRects())
	{
		if (rect.x >= tableWidth && rect.y >= screen->Height * 40 / 100 && rect.y < best)
		{
			best = rect.y;
			split = rect.y - 8;
		}
	}
	logo = SDL_Rect{tableWidth, 0, sidebarWidth, split};
	scores = SDL_Rect{tableWidth, split, sidebarWidth, screen->Height - split};
}

void modernfx::FitRect(const SDL_Rect& src, const SDL_Rect& area, float maxScale, SDL_Rect& dst)
{
	auto scale = std::min(static_cast<float>(area.w) / src.w, static_cast<float>(area.h) / src.h);
	scale = std::min(scale, maxScale);
	dst.w = static_cast<int>(src.w * scale);
	dst.h = static_cast<int>(src.h * scale);
	dst.x = area.x + (area.w - dst.w) / 2;
	dst.y = area.y + (area.h - dst.h) / 2;
}

void modernfx::DrawAmbient(SDL_Texture* screenTexture, bool wide)
{
	if (!wide || !screenTexture)
		return;

	// Render the table into a tiny texture and stretch it: a cheap blur behind the side panels.
	if (!Ambient && SDL_RenderTargetSupported(winmain::Renderer))
	{
		Ambient = SDL_CreateTexture(winmain::Renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, 32, 24);
#if SDL_VERSION_ATLEAST(2, 0, 12)
		if (Ambient)
			SDL_SetTextureScaleMode(Ambient, SDL_ScaleModeLinear);
#endif
	}
	if (!Ambient || SDL_SetRenderTarget(winmain::Renderer, Ambient) != 0)
		return;
	SDL_RenderCopy(winmain::Renderer, screenTexture, nullptr, nullptr);
	SDL_SetRenderTarget(winmain::Renderer, nullptr);

	int width, height;
	SDL_GetRendererOutputSize(winmain::Renderer, &width, &height);
	SDL_SetTextureColorMod(Ambient, 55, 55, 70);
	SDL_Rect full{0, winmain::MainMenuHeight, width, height - winmain::MainMenuHeight};
	SDL_RenderCopy(winmain::Renderer, Ambient, nullptr, &full);
}

void modernfx::DrawPanels(SDL_Texture* screenTexture, int hdScale, const SDL_Rect& screenRect, const SDL_Rect& left,
                          const SDL_Rect& right)
{
	if (!screenTexture || !render::vscreen)
		return;

	// Backglass on the left, enlarged score panel on the right, each in a dark frame.
	SDL_Rect logo, scores;
	PanelRegions(logo, scores);
	auto tableScale = static_cast<float>(screenRect.h) / render::vscreen->Height;
	const SDL_Rect* areas[2] = {&left, &right};
	const SDL_Rect* regions[2] = {&logo, &scores};
	for (int i = 0; i < 2; i++)
	{
		SDL_Rect area = *areas[i];
		auto margin = std::max(12, area.w / 12);
		area.x += margin;
		area.w -= margin * 2;
		area.y += margin;
		area.h -= margin * 2;
		if (area.w <= 0 || area.h <= 0)
			continue;
		SDL_Rect dst;
		FitRect(*regions[i], area, tableScale * 2.0f, dst);
		if (i == 0)
			dst.y = area.y + std::min(area.h / 10, area.h - dst.h);

		SDL_Rect frame{dst.x - 8, dst.y - 8, dst.w + 16, dst.h + 16};
		SDL_SetRenderDrawBlendMode(winmain::Renderer, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(winmain::Renderer, 0, 0, 0, 210);
		SDL_RenderFillRect(winmain::Renderer, &frame);
		SDL_SetRenderDrawColor(winmain::Renderer, 130, 115, 190, 255);
		SDL_RenderDrawRect(winmain::Renderer, &frame);
		SDL_SetRenderDrawColor(winmain::Renderer, 0, 0, 0, 255);
		SDL_SetRenderDrawBlendMode(winmain::Renderer, SDL_BLENDMODE_NONE);

		SDL_Rect src{
			regions[i]->x * hdScale, regions[i]->y * hdScale, regions[i]->w * hdScale, regions[i]->h * hdScale
		};
		SDL_RenderCopy(winmain::Renderer, screenTexture, &src, &dst);

		// Dot-matrix boxes drawn again at the panel's size.
		auto s = static_cast<float>(dst.w) / regions[i]->w;
		ledfx::DrawDisplays(dst.x - regions[i]->x * s, dst.y - regions[i]->y * s, s, s, *regions[i]);
	}
}

void modernfx::DrawEffects()
{
	if (!render::vscreen)
		return;
	auto dot = GetDot();
	if (!dot)
		return;
	float originX, originY, scaleX, scaleY;
	SDL_Rect clip;
	render::GetBoardTransform(originX, originY, scaleX, scaleY, clip);
	SDL_RenderSetClipRect(winmain::Renderer, &clip);

	auto drawDot = [&](float x, float y, float size, const uint8_t* color, float alpha)
	{
		if (alpha <= 0.004f)
			return;
		SDL_SetTextureColorMod(dot, color[0], color[1], color[2]);
		SDL_SetTextureAlphaMod(dot, static_cast<uint8_t>(std::min(1.0f, alpha) * 255));
		SDL_Rect dst{
			static_cast<int>(originX + (x - size / 2) * scaleX), static_cast<int>(originY + (y - size / 2) * scaleY),
			std::max(1, static_cast<int>(size * scaleX + 0.5f)), std::max(1, static_cast<int>(size * scaleY + 0.5f))
		};
		SDL_RenderCopy(winmain::Renderer, dot, nullptr, &dst);
	};

	// Ball trail.
	if (options::Options.BallTrail)
	{
		const uint8_t trailColor[3] = {170, 200, 255};
		for (auto ball : render::GetBalls())
		{
			auto& trail = Trails[ball];
			if (!ball->Bmp || ball->DirtyRect.Width <= 0)
			{
				trail.clear();
				continue;
			}
			auto radius = ball->BmpRect.Width / 2.0f;
			auto bx = ball->BmpRect.XPosition + radius, by = ball->BmpRect.YPosition + ball->BmpRect.Height / 2.0f;
			if (!trail.empty())
			{
				auto jump = std::hypot(bx - trail[0], by - trail[1]);
				if (jump > radius * 8)
					trail.clear();
			}
			trail.insert(trail.begin(), {bx, by, static_cast<float>(ball->Depth)});
			if (trail.size() > TrailLength * 3)
				trail.resize(TrailLength * 3);

			for (auto i = trail.size() / 3 - 1; i >= 1; i--)
			{
				auto x = trail[i * 3], y = trail[i * 3 + 1];
				if (std::hypot(x - bx, y - by) < radius * 0.9f)
					continue;
				if (render::DepthAt(static_cast<int>(x), static_cast<int>(y)) < trail[i * 3 + 2])
					continue;
				auto t = static_cast<float>(i) / TrailLength;
				drawDot(x, y, radius * 2.2f * (1.0f - t * 0.6f), trailColor, 0.28f * (1.0f - t));
			}
		}
	}
	else
	{
		Trails.clear();
	}

	// Bumper flashes and sparks.
	for (auto& p : Particles)
	{
		auto t = p.Life / p.MaxLife;
		auto isFlash = p.VX == 0 && p.VY == 0;
		drawDot(p.X, p.Y, isFlash ? p.Size : p.Size * (0.5f + 0.5f * t), p.Color, isFlash ? 0.55f * t : t);
	}

	SDL_RenderSetClipRect(winmain::Renderer, nullptr);
}

void modernfx::DrawCrt()
{
	if (!options::Options.CrtFilter)
		return;

	int width, height;
	if (SDL_GetRendererOutputSize(winmain::Renderer, &width, &height) != 0)
		return;
	auto top = winmain::MainMenuHeight;
	auto areaH = height - top;
	if (areaH <= 0)
		return;

	// Scanlines: every third screen row darker, applied by multiplying.
	if (!Scanlines || ScanlinesHeight != areaH)
	{
		if (Scanlines)
			SDL_DestroyTexture(Scanlines);
		std::vector<ColorRgba> pixels(areaH);
		for (int y = 0; y < areaH; y++)
		{
			uint8_t v = y % 3 == 2 ? 165 : 255;
			pixels[y] = ColorRgba{v, v, v, 255};
		}
		Scanlines = CreateTexture(pixels, 1, areaH, SDL_BLENDMODE_MOD, false);
		ScanlinesHeight = areaH;
	}
	if (!Vignette)
	{
		constexpr int size = 64;
		std::vector<ColorRgba> pixels(size * size);
		for (int y = 0; y < size; y++)
			for (int x = 0; x < size; x++)
			{
				auto dx = (x + 0.5f) / size * 2 - 1, dy = (y + 0.5f) / size * 2 - 1;
				auto r = std::sqrt(dx * dx + dy * dy);
				auto t = std::min(1.0f, std::max(0.0f, (r - 0.6f) / 0.8f));
				auto v = static_cast<uint8_t>(255 * (1.0f - 0.5f * t * t));
				pixels[y * size + x] = ColorRgba{v, v, v, 255};
			}
		Vignette = CreateTexture(pixels, size, size, SDL_BLENDMODE_MOD, true);
	}

	SDL_Rect full{0, top, width, areaH};
	if (Scanlines)
		SDL_RenderCopy(winmain::Renderer, Scanlines, nullptr, &full);
	if (Vignette)
		SDL_RenderCopy(winmain::Renderer, Vignette, nullptr, &full);
}

void modernfx::RenderTitleButton()
{
	// Shown on the start screen: no game running, the demo playing, or a fresh game before any points.
	auto table = pb::MainTable;
	auto fresh = table && table->CurScoreStruct && table->CurScoreStruct->Score <= 0;
	if (SettingsOpen || !render::vscreen || !table ||
		(pb::game_mode == GameModes::InGame && !winmain::DemoActive && !fresh))
		return;

	// Bottom centre of the table, over the apron.
	float originX, originY, scaleX, scaleY;
	SDL_Rect clip;
	render::GetBoardTransform(originX, originY, scaleX, scaleY, clip);
	auto centerX = clip.x + clip.w / 2.0f, bottom = clip.y + clip.h - 12.0f * scaleY;
	SDL_Rect wideRect, left, right;
	if (WideLayout(wideRect, left, right))
	{
		centerX = (left.w + right.x) / 2.0f;
		bottom = static_cast<float>(wideRect.y + wideRect.h) - 12.0f * wideRect.h / render::vscreen->Height;
	}
	ImGui::SetNextWindowPos(ImVec2(centerX, bottom), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
	ImGui::SetNextWindowBgAlpha(0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	if (ImGui::Begin("##TitleSettings", nullptr,
	                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
	                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
	                 ImGuiWindowFlags_NoNav))
	{
		ImGui::SetWindowFontScale(1.4f);
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.12f, 0.35f, 0.85f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f, 0.22f, 0.6f, 0.95f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.45f, 0.3f, 0.8f, 1.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(18, 8));
		if (ImGui::Button("Settings"))
			SettingsOpen = true;
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor(3);
	}
	ImGui::End();
	ImGui::PopStyleVar();
}

void modernfx::RenderSettingsWindow()
{
	if (!SettingsOpen)
		return;

	auto& opt = options::Options;
	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	if (ImGui::Begin("Settings", &SettingsOpen, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse))
	{
		auto heading = [](const char* text)
		{
			ImGui::Spacing();
			ImGui::TextColored(ImVec4(0.75f, 0.65f, 1.0f, 1.0f), "%s", text);
			ImGui::Separator();
		};

		heading("Lighting");
		if (ImGui::Checkbox("LED lighting (smooth fades, LED colours)", &opt.LedLighting.V))
			ledfx::SettingsChanged();
		ImGui::Checkbox("Light glow", &opt.LightGlow.V);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(140);
		ImGui::SliderFloat("##Glow strength", &opt.LightGlowStrength.V, 0.0f, 2.0f, "strength %.2f",
		                   ImGuiSliderFlags_AlwaysClamp);
		ImGui::Checkbox("Light spill onto the table", &opt.LightSpill.V);
		ImGui::Checkbox("Dot-matrix score display", &opt.DotMatrix.V);

		heading("Ball and motion");
		ImGui::Checkbox("Ball shadow", &opt.BallShadow.V);
		ImGui::Checkbox("Ball light reflections", &opt.BallGlints.V);
		ImGui::Checkbox("Ball trail", &opt.BallTrail.V);
		ImGui::Checkbox("Bumper flash and sparks", &opt.BumperSparks.V);
		ImGui::Checkbox("Shake the table when nudging", &opt.NudgeShake.V);

		heading("Screen");
		ImGui::Checkbox("Widescreen backglass (wide windows and full screen)", &opt.Backglass.V);
		ImGui::Checkbox("CRT / arcade filter", &opt.CrtFilter.V);
		if (ImGui::Checkbox("Smooth play (match monitor refresh, 240 physics updates/s)", &opt.SmoothPlay.V))
			winmain::UpdateFrameRate();

		ImGui::Spacing();
		if (ImGui::Button("Close", ImVec2(120, 0)))
			SettingsOpen = false;
	}
	ImGui::End();
}

void modernfx::Uninit()
{
	for (auto texture : {Dot, Ambient, Scanlines, Vignette})
	{
		if (texture)
			SDL_DestroyTexture(texture);
	}
	Dot = Ambient = Scanlines = Vignette = nullptr;
	ScanlinesHeight = 0;
	Particles.clear();
	Trails.clear();
}
