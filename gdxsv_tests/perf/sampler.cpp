// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Sampling profiler for Windows: suspends each thread of a process about every millisecond and records its
// instruction pointer, until the process exits.
// usage: sampler <pid> <out.txt>
// out.txt: "M <base> <size> <module>" lines, then "<thread id> <rip> <count>" lines (hex addresses).
// build: g++ -O2 -std=c++17 sampler.cpp -o sampler.exe -lwinmm
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <tuple>
#include <vector>

int main(int argc, char** argv) {
	if (argc != 3) {
		fprintf(stderr, "usage: sampler <pid> <out.txt>\n");
		return 2;
	}
	DWORD pid = strtoul(argv[1], nullptr, 10);
	HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
	if (!proc) {
		fprintf(stderr, "cannot open process %lu\n", pid);
		return 1;
	}
	timeBeginPeriod(1);
	std::map<std::pair<DWORD, DWORD64>, int> hist;
	std::map<DWORD, HANDLE> threads;
	std::vector<std::tuple<unsigned long long, unsigned long, std::string>> modules;
	int n = 0;
	while (WaitForSingleObject(proc, 0) == WAIT_TIMEOUT) {
		if (n % 200 == 0) {  // new threads and modules; the module list is gone once the process exits
			HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
			THREADENTRY32 te{sizeof(te)};
			for (BOOL ok = Thread32First(s, &te); ok; ok = Thread32Next(s, &te))
				if (te.th32OwnerProcessID == pid && !threads.count(te.th32ThreadID)) {
					HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
										  te.th32ThreadID);
					if (h) threads[te.th32ThreadID] = h;
				}
			CloseHandle(s);
			HANDLE ms = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
			if (ms != INVALID_HANDLE_VALUE) {
				modules.clear();
				MODULEENTRY32 me{sizeof(me)};
				for (BOOL ok = Module32First(ms, &me); ok; ok = Module32Next(ms, &me))
					modules.emplace_back((unsigned long long)me.modBaseAddr, me.modBaseSize, me.szModule);
				CloseHandle(ms);
			}
		}
		for (auto& [tid, h] : threads) {
			if (SuspendThread(h) == (DWORD)-1) continue;
			CONTEXT c{};
			c.ContextFlags = CONTEXT_CONTROL;
			if (GetThreadContext(h, &c)) hist[{tid, c.Rip}]++;
			ResumeThread(h);
		}
		n++;
		Sleep(1);
	}
	FILE* o = fopen(argv[2], "w");
	if (!o) return 1;
	for (auto& [base, size, name] : modules) fprintf(o, "M %llx %lx %s\n", base, size, name.c_str());
	for (auto& [k, v] : hist) fprintf(o, "%lu %llx %d\n", k.first, k.second, v);
	fclose(o);
	printf("samples %d threads %zu\n", n, threads.size());
	return 0;
}
