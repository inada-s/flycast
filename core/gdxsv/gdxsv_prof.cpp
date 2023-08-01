#include "gdxsv_prof.h"
#include "log/LogManager.h"

void GdxsvProf::Print() {
	for (auto& p : duration_) {
		NOTICE_LOG(COMMON, "%s\t%ld[ms]", p.first.c_str(), std::chrono::duration_cast<std::chrono::milliseconds>(p.second).count());
	}
}

GdxsvProf gdxsv_prof;
