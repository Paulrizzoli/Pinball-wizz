#include "pch.h"
#include "customrules.h"

#include "control.h"
#include "customtable.h"
#include "pb.h"
#include "TBall.h"
#include "TDrain.h"
#include "TFlipper.h"
#include "TLightGroup.h"
#include "TPinballTable.h"
#include "TPlunger.h"
#include "TTextBox.h"
#include "translations.h"
#include "timer.h"
#include "TKickout.h"
#include "TPopupTarget.h"
#include "TTripwire.h"

TPinballTable* customrules::Table = nullptr;
bool customrules::LastBall = false;
int customrules::BallBonus = 0;
int customrules::ActivePlayer = 0;
float customrules::BallStartTime = -1;
bool customrules::BallSaved = false;
std::vector<customrules::GroupState> customrules::Groups[4];
std::map<int, TPinballComponent*> customrules::Parts;

namespace
{
	const int Multipliers[5] = {1, 2, 3, 5, 10};

	// When each lane switch last counted, so a ball rolling along one only counts once.
	std::map<TPinballComponent*, float> LaneHitTimes;

	std::string Thousands(int value)
	{
		auto digits = std::to_string(value);
		std::string out;
		for (size_t i = 0; i < digits.size(); i++)
		{
			if (i && (digits.size() - i) % 3 == 0)
				out += ',';
			out += digits[i];
		}
		return out;
	}
}

void customrules::Attach(TPinballTable* table)
{
	Table = table;
	LastBall = false;
	BallBonus = 0;
	ActivePlayer = 0;
	BallStartTime = -1;
	BallSaved = false;
	Parts.clear();
	for (auto component : table->ComponentList)
		if (component->GroupIndex >= 0)
			Parts[component->GroupIndex] = component;
	for (int player = 0; player < 4; player++)
		ResetGroups(player);
}

void customrules::Detach()
{
	Table = nullptr;
	Parts.clear();
	LaneHitTimes.clear();
	timer::kill(MissionTimeUp);
	for (auto& player : Groups)
		for (auto& state : player)
			state.Timer = 0;
}

void customrules::ResetGroups(int player)
{
	auto& groups = Groups[player];
	// A mission still running from the last game must not time out in the new one.
	for (auto& state : groups)
	{
		if (state.Timer)
			timer::kill(state.Timer);
	}
	groups.clear();
	for (auto& group : customtable::Rules.Groups)
	{
		GroupState state;
		state.Done.assign(group.Parts.size(), false);
		groups.push_back(state);
	}
}

void customrules::Info(const std::string& text, float seconds)
{
	if (pb::InfoTextBox)
		pb::InfoTextBox->Display(text.c_str(), seconds);
}

void customrules::Mission(const std::string& text, float seconds)
{
	// The newest message replaces the one showing, so quick hits don't build up a queue.
	if (pb::MissTextBox)
	{
		pb::MissTextBox->Clear();
		pb::MissTextBox->Display(text.c_str(), seconds);
	}
}

void customrules::SetLight(int group, bool on)
{
	auto it = Parts.find(group);
	if (it != Parts.end())
		it->second->Message(on ? MessageCode::TLightResetAndTurnOn : MessageCode::TLightResetAndTurnOff, 0.0f);
}

void customrules::FlashLight(int group, float seconds)
{
	auto it = Parts.find(group);
	if (it != Parts.end())
		it->second->Message(MessageCode::TLightFlasherStartTimedThenStayOff, seconds);
}

void customrules::ShowMultiplierLights()
{
	auto& lights = customtable::Rules.MultiplierLights;
	for (size_t i = 0; i < lights.size(); i++)
		SetLight(lights[i], static_cast<int>(i) < Table->ScoreMultiplier);
}

void customrules::Handler(MessageCode code, TPinballComponent* caller)
{
	if (!Table || !caller)
		return;

	switch (code)
	{
	case MessageCode::ControlCollision:
		if (caller == Table->Drain)
		{
			Drained();
			break;
		}
		if (!Table->TiltLockFlag)
			PartHit(caller);
		break;
	case MessageCode::ControlTimerExpired:
		if (caller != Table->Drain)
			break;
		if (LastBall)
		{
			LastBall = false;
			Table->Message(MessageCode::GameOver, 0.0);
			if (pb::chk_highscore() && pb::MissTextBox)
				pb::MissTextBox->Display(pb::get_rc_string(Msg::STRING277), -1.0);
		}
		else
		{
			Table->Plunger->Message(MessageCode::PlungerStartFeedTimer, 0.0);
		}
		break;
	case MessageCode::ControlMissionStarted:
		// Shown while waiting for a new game.
		if (pb::MissTextBox && caller == pb::MissTextBox)
			pb::MissTextBox->Display(customtable::Name.c_str(), -1.0);
		break;
	default:
		break;
	}
}

void customrules::PartHit(TPinballComponent* part)
{
	if (dynamic_cast<TTripwire*>(part))
	{
		auto last = LaneHitTimes.find(part);
		if (last != LaneHitTimes.end() && pb::time_now - last->second < 0.4f && pb::time_now >= last->second)
			return;
		LaneHitTimes[part] = pb::time_now;
	}
	if (BallStartTime < 0)
		BallStartTime = pb::time_now;
	auto index = part->GroupIndex;
	auto score = customtable::ScoreFor(part);
	if (score > 0)
		Table->AddScore(score);
	auto bonus = customtable::Bonus.find(index);
	if (bonus != customtable::Bonus.end())
		BallBonus += bonus->second;

	auto& rules = customtable::Rules.Groups;
	auto& states = Groups[ActivePlayer];
	for (size_t g = 0; g < rules.size() && g < states.size(); g++)
	{
		auto& rule = rules[g];
		auto& state = states[g];
		if (rule.IsMission && !state.Running)
			continue;
		for (size_t i = 0; i < rule.Parts.size(); i++)
		{
			if (rule.Parts[i] != index)
				continue;
			if (rule.Hits <= 0)
			{
				if (state.Done[i])
					break;
				state.Done[i] = true;
				if (i < rule.Lights.size())
					SetLight(rule.Lights[i], true);
				auto done = static_cast<int>(std::count(state.Done.begin(), state.Done.end(), true));
				auto total = static_cast<int>(state.Done.size());
				if (done >= total)
					Complete(g);
				else
					Mission(rule.Name + "  " + std::to_string(done) + " of " + std::to_string(total), 2.0f);
			}
			else
			{
				state.Hits++;
				// Lights fill up as hits add up.
				auto lights = rule.Lights.size();
				for (size_t l = 0; l < lights; l++)
					SetLight(rule.Lights[l], state.Hits * static_cast<int>(lights) >= rule.Hits * static_cast<int>(l + 1));
				if (state.Hits >= rule.Hits)
					Complete(g);
				else
					Mission(rule.Name + "  " + std::to_string(state.Hits) + " of " + std::to_string(rule.Hits), 1.5f);
			}
			break;
		}
	}

	// A kickout hole holds the ball for a moment, then kicks it out.
	if (dynamic_cast<TKickout*>(part))
		part->Message(MessageCode::TKickoutRestartTimer, -1.0f);
}

void customrules::Complete(size_t groupIndex)
{
	auto& rule = customtable::Rules.Groups[groupIndex];
	auto& state = Groups[ActivePlayer][groupIndex];
	state.Done.assign(state.Done.size(), false);
	state.Hits = 0;
	if (rule.IsMission)
		EndMission(groupIndex, false);
	for (auto light : rule.Lights)
		FlashLight(light, 1.5f);
	// Drop targets come back up for the next round.
	for (auto partGroup : rule.Parts)
	{
		auto it = Parts.find(partGroup);
		if (it != Parts.end() && dynamic_cast<TPopupTarget*>(it->second))
			it->second->Message(MessageCode::TPopupTargetEnable, 0.0f);
	}

	auto added = rule.Score > 0 ? Table->AddScore(rule.Score) : 0;
	BallBonus += rule.Bonus;

	std::string award;
	if (rule.Award == "multiplier")
	{
		if (Table->ScoreMultiplier < 4)
			Table->ScoreMultiplier++;
		award = "MULTIPLIER " + std::to_string(Multipliers[Table->ScoreMultiplier]) + "X";
		ShowMultiplierLights();
	}
	else if (rule.Award == "extraBall")
	{
		Table->ExtraBalls++;
		award = "EXTRA BALL";
		SetLight(customtable::Rules.ExtraBallLight, true);
	}
	else if (rule.Award == "multiball")
	{
		if (Table->MultiballCount <= 1)
		{
			Multiball();
			award = "MULTIBALL";
		}
	}
	else if (rule.Award == "startMission" && rule.StartsMission >= 0)
	{
		auto& other = Groups[ActivePlayer][rule.StartsMission];
		if (!other.Running)
		{
			StartMission(static_cast<size_t>(rule.StartsMission));
			return; // the mission's own message is shown instead
		}
	}

	auto message = rule.Message.empty() ? rule.Name + " COMPLETE" : rule.Message;
	if (!award.empty())
	{
		if (message.find(award) == std::string::npos)
			message += "\n" + award;
	}
	else if (added > 0)
		message += "\n" + Thousands(added);
	Mission(message, 3.0f);
}

void customrules::Drained()
{
	// Other balls still in play.
	if (!Table->BallInDrainFlag)
		return;

	if (control::table_unlimited_balls)
	{
		LastBall = false;
		return;
	}

	// Ball save: a ball lost soon after it started playing (or before it hit anything) comes back, once.
	auto save = customtable::Rules.BallSave;
	if (save > 0 && !BallSaved && !Table->TiltLockFlag &&
		(BallStartTime < 0 || pb::time_now - BallStartTime <= save))
	{
		BallSaved = true;
		BallStartTime = -1;
		LastBall = false;
		Mission("BALL SAVED", 2.0f);
		return;
	}
	BallSaved = false;
	BallStartTime = -1;
	EndMissions();

	// End-of-ball bonus, times the multiplier.
	if (!Table->TiltLockFlag && BallBonus > 0)
	{
		auto added = Table->AddScore(BallBonus);
		Mission("BONUS " + Thousands(added), 2.0f);
	}
	BallBonus = 0;
	Table->ScoreMultiplier = 0;
	ShowMultiplierLights();

	if (Table->TiltLockFlag)
		Table->Message(MessageCode::ClearTiltLock, 0.0);

	if (Table->ExtraBalls)
	{
		Table->ExtraBalls--;
		if (!Table->ExtraBalls)
			SetLight(customtable::Rules.ExtraBallLight, false);
		Info(pb::get_rc_string(Msg::STRING198), -1.0f);
		LastBall = false;
		return;
	}

	Table->ChangeBallCount(Table->BallCount - 1);
	if (Table->CurrentPlayer + 1 != Table->PlayerCount || Table->BallCount)
	{
		Table->Message(MessageCode::SwitchToNextPlayer, 0.0);
		LastBall = false;
	}
	else
	{
		LastBall = true;
	}
}

void customrules::TableMessage(MessageCode code)
{
	if (!Table)
		return;
	switch (code)
	{
	case MessageCode::Reset:
		LastBall = false;
		BallBonus = 0;
		BallStartTime = -1;
		BallSaved = false;
		ActivePlayer = 0;
		for (int player = 0; player < 4; player++)
			ResetGroups(player);
		break;
	case MessageCode::SwitchToNextPlayer:
		// Each player keeps their own group progress; the lights keep their own state per player.
		ActivePlayer = std::max(0, std::min(3, Table->CurrentPlayer));
		break;
	default:
		break;
	}
}

void customrules::UnstuckBall(TBall& ball, int dt)
{
	if (!Table)
		return;
	if (control::CheckBallInControlBounds(ball, *Table->FlipperL) ||
		control::CheckBallInControlBounds(ball, *Table->FlipperR) ||
		control::CheckBallInControlBounds(ball, *Table->Plunger))
		return;

	if (ball.StuckCounter <= 20)
	{
		vector3 throwDir{0.0f, -1.0f, 0.0f};
		ball.throw_ball(&throwDir, 90.0f, 1.0f, 0.0f);
	}
	else
	{
		ball.Disable();
		Table->MultiballCount--;
		Table->Plunger->Message(MessageCode::PlungerRelaunchBall, 0);
	}
}

void customrules::StartMission(size_t groupIndex)
{
	auto& rule = customtable::Rules.Groups[groupIndex];
	auto& state = Groups[ActivePlayer][groupIndex];
	state.Running = true;
	state.Done.assign(state.Done.size(), false);
	state.Hits = 0;
	for (auto light : rule.Lights)
	{
		auto it = Parts.find(light);
		if (it != Parts.end())
			it->second->Message(MessageCode::TLightFlasherStart, 0.0f);
	}
	if (state.Timer)
		timer::kill(state.Timer);
	state.Timer = 0;
	if (rule.Time > 0)
		state.Timer = timer::set(rule.Time, reinterpret_cast<void*>(groupIndex + 1), MissionTimeUp);
	auto text = rule.Name;
	if (!rule.Hint.empty())
		text += "\n" + rule.Hint;
	Mission(text, 4.0f);
}

void customrules::EndMission(size_t groupIndex, bool failed)
{
	auto& rule = customtable::Rules.Groups[groupIndex];
	auto& state = Groups[ActivePlayer][groupIndex];
	if (!state.Running)
		return;
	state.Running = false;
	if (state.Timer)
		timer::kill(state.Timer);
	state.Timer = 0;
	state.Done.assign(state.Done.size(), false);
	state.Hits = 0;
	if (failed)
	{
		for (auto light : rule.Lights)
			SetLight(light, false);
		Mission(rule.Name + "\nTIME UP", 2.5f);
	}
}

void customrules::EndMissions()
{
	auto& rules = customtable::Rules.Groups;
	for (size_t g = 0; g < rules.size() && g < Groups[ActivePlayer].size(); g++)
	{
		if (Groups[ActivePlayer][g].Running)
		{
			EndMission(g, false);
			for (auto light : rules[g].Lights)
				SetLight(light, false);
		}
	}
}

void customrules::MissionTimeUp(int timerId, void* caller)
{
	auto groupIndex = reinterpret_cast<size_t>(caller) - 1;
	if (!Table || groupIndex >= Groups[ActivePlayer].size())
		return;
	// The timer has fired; forget it before ending so it isn't killed twice.
	Groups[ActivePlayer][groupIndex].Timer = 0;
	EndMission(groupIndex, true);
}

void customrules::Multiball()
{
	// A second ball shot from the plunger lane.
	auto pos = Table->PlungerPosition;
	if (Table->BallCountInRect(pos, Table->CollisionCompOffset * 1.2f))
		return;
	auto ball = Table->AddBall(pos);
	if (!ball)
		return;
	Table->MultiballCount++;
	vector3 up{0.0f, -1.0f, 0.0f};
	ball->throw_ball(&up, 0.0f, 32.0f, 0.0f);
}
