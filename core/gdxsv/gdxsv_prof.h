#pragma once
#include <chrono>
#include <map>
#include <string>

class GdxsvProf
{
public:
	void Start(const char* name) {
		if (t0_.find(name) != t0_.end()) return;
		t0_[name] = std::chrono::high_resolution_clock::now();
	}

	void Stop(const char* name) {
		if (t0_.find(name) == t0_.end()) return;
		duration_[name] += std::chrono::high_resolution_clock::now() - t0_[name];
		t0_.erase(name);
	}

	void Print();

	void Reset() {
		t0_.clear();
		duration_.clear();
	}

private:
	std::map<std::string, std::chrono::high_resolution_clock::time_point> t0_;
	std::map<std::string, std::chrono::high_resolution_clock::duration> duration_;
};

extern GdxsvProf gdxsv_prof;
