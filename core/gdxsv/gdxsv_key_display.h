#pragma once

#include <array>
#include <deque>

#include "types.h"

// Scale of the game image (640x480 units) in a w x h display: the OSD overlays grow with it.
float gdxsv_osd_scale(float w, float h);

class GdxsvKeyDisplay {
   public:
	void DisplayOSD();
	void AppendInput(int player, u16 mcs_key);
	void SetDisplayPlayer(int player);
	void Clear();

	void enabled(bool enabled) { enabled_ = enabled; }
	bool enabled() const { return enabled_; }

   private:
	struct McsPadInput {
		int frame;
		u16 code;
	};

	bool enabled_ = false;
	int display_player_ = 0;
	std::array<std::deque<McsPadInput>, 4> history_;
};
