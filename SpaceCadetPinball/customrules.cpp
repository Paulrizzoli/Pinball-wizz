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

TPinballTable* customrules::Table = nullptr;
bool customrules::LastBall = false;

void customrules::Attach(TPinballTable* table)
{
	Table = table;
	LastBall = false;
}

void customrules::Detach()
{
	Table = nullptr;
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
		{
			auto score = customtable::ScoreFor(caller);
			if (score > 0)
				Table->AddScore(score);
		}
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

	if (Table->TiltLockFlag)
		Table->Message(MessageCode::ClearTiltLock, 0.0);

	if (Table->ExtraBalls)
	{
		Table->ExtraBalls--;
		if (pb::InfoTextBox)
			pb::InfoTextBox->Display(pb::get_rc_string(Msg::STRING198), -1.0);
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
