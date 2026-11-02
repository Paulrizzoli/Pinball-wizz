#include "pch.h"
#include "TBumper.h"


#include "control.h"
#include "loader.h"
#include "render.h"
#include "timer.h"
#include "TPinballTable.h"
#include "ledfx.h"

TBumper::TBumper(TPinballTable* table, int groupIndex) : TCollisionComponent(table, groupIndex, true)
{
	visualStruct visual{};

	BmpIndex = 0;
	Timer = 0;
	TimerTime = *loader::query_float_attribute(groupIndex, 0, 407);
	loader::query_visual(groupIndex, 0, &visual);
	SoundIndex4 = visual.SoundIndex4;
	SoundIndex3 = visual.SoundIndex3;
	OriginalThreshold = Threshold;
	if (RenderSprite && ListBitmap)
	{
		// Bumper bitmaps come in unlit/lit pairs; only the difference within a pair is lamp light.
		ledfx::Register(RenderSprite, false);
		ledfx::MarkBumper(RenderSprite);
		for (auto index = 0u; index < ListBitmap->size(); index++)
		{
			auto unlit = ListBitmap->at(index & ~1u).Bmp;
			ledfx::SetUnlit(ListBitmap->at(index).Bmp,
			                {unlit, unlit->XPosition - table->XOffset, unlit->YPosition - table->YOffset});
		}
	}
}

int TBumper::Message(MessageCode code, float value)
{
	switch (code)
	{
	case MessageCode::TBumperSetBmpIndex:
		{
			auto nextBmp = static_cast<int>(floor(value));
			auto maxBmp = static_cast<int>(ListBitmap->size()) - 1;
			if (2 * nextBmp > maxBmp)
				nextBmp = maxBmp / 2;
			if (nextBmp < 0)
				nextBmp = 0;
			if (nextBmp != BmpIndex)
			{
				if (nextBmp >= BmpIndex)
					loader::play_sound(SoundIndex4, this, "TBumper1");
				if (nextBmp < BmpIndex)
					loader::play_sound(SoundIndex3, this, "TBumper2");
				BmpIndex = nextBmp;
				Fire();
				control::handler(MessageCode::TBumperSetBmpIndex, this);
			}
			break;
		}
	case MessageCode::TBumperIncBmpIndex:
		{
			auto nextBmp = BmpIndex + 1;
			auto maxBmp = static_cast<int>(ListBitmap->size()) - 1;
			if (2 * nextBmp > maxBmp)
				nextBmp = maxBmp / 2;
			TBumper::Message(MessageCode::TBumperSetBmpIndex, static_cast<float>(nextBmp));
			break;
		}
	case MessageCode::TBumperDecBmpIndex:
		{
			auto nextBmp = BmpIndex - 1;
			if (nextBmp < 0)
				nextBmp = 0;
			TBumper::Message(MessageCode::TBumperSetBmpIndex, static_cast<float>(nextBmp));
			break;
		}
	case MessageCode::PlayerChanged:
		{
			auto playerPtr = &PlayerData[PinballTable->CurrentPlayer];
			playerPtr->BmpIndex = BmpIndex;
			playerPtr->MessageField = MessageField;

			playerPtr = &PlayerData[static_cast<int>(floor(value))];
			BmpIndex = playerPtr->BmpIndex;
			MessageField = playerPtr->MessageField;
			TBumper::Message(MessageCode::TBumperSetBmpIndex, static_cast<float>(BmpIndex));
			break;
		}
	case MessageCode::Reset:
		{
			if (Timer)
			{
				timer::kill(Timer);
				TimerExpired(Timer, this);
			}
			BmpIndex = 0;
			MessageField = 0;
			for (auto& playerPtr : PlayerData)
			{
				playerPtr.BmpIndex = 0;
				playerPtr.MessageField = 0;
			}
			TimerExpired(0, this);
			break;
		}
	default:
		break;
	}

	return 0;
}

void TBumper::Collision(TBall* ball, vector2* nextPosition, vector2* direction, float distance, TEdgeSegment* edge)
{
	if (DefaultCollision(ball, nextPosition, direction))
	{
		Fire();
		control::handler(MessageCode::ControlCollision, this);
	}
}

void TBumper::TimerExpired(int timerId, void* caller)
{
	auto bump = static_cast<TBumper*>(caller);
	bump->SpriteSet(bump->BmpIndex * 2);
	bump->Timer = 0;
	bump->Threshold = bump->OriginalThreshold;
}

void TBumper::Fire()
{
	SpriteSet(2 * BmpIndex + 1);
	Timer = timer::set(TimerTime, this, TimerExpired);
	Threshold = 1000000000.0;
}
