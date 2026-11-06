#include "gdxsv_save_state.h"

#include "gdxsv_camera.h"
#include "serialize.h"

void GdxsvSaveState::StartUsing() { enabled = true; }

void GdxsvSaveState::EndUsing() { enabled = false; }

bool GdxsvSaveState::SaveState(int frame) {
	if (buffers.find(frame) != buffers.end()) {
		// Already saved
		return true;
	}
	// TODO this is way too much memory
	size_t allocSize = (settings.platform.isNaomi() ? 20 : 10) * 1024 * 1024;
	auto buffer = (unsigned char*)malloc(allocSize);
	if (buffer == nullptr) {
		ERROR_LOG(NETWORK, "Memory alloc failed");
		return false;
	}
	Serializer ser(buffer, allocSize, true);
	ser << frame;
	dc_serialize(ser);
	verify(ser.size() < allocSize);
	memwatch::protect();
	if (!buffers.empty()) {
		deltaStates[buffers.rbegin()->first].load();
	}
	buffers[frame] = std::make_pair(ser.size(), buffer);
	return true;
}

bool GdxsvSaveState::LoadState(int frame) {
	if (buffers.find(frame) == buffers.end()) {
		ERROR_LOG(COMMON, "GdxsvSaveState.LoadState: no frame info");
		return false;
	}
	return LoadStateInternal(frame);
}

bool GdxsvSaveState::LoadStateMostRecent(int& frame) {
	if (buffers.empty()) {
		return false;
	}
	auto it = buffers.lower_bound(frame);
	if (it != buffers.begin()) {
		it = std::prev(it);
	}
	if (it == buffers.begin()) {
		return false;
	}
	frame = it->first;
	return LoadStateInternal(it->first);
}

int GdxsvSaveState::FindSavedFrameAtOrBefore(int frame) const {
	if (buffers.empty()) {
		return -1;
	}
	auto it = buffers.upper_bound(frame);
	if (it == buffers.begin()) {
		return -1;
	}
	return std::prev(it)->first;
}

bool GdxsvSaveState::LoadStateInternal(int frame) {
	if (LastSavedFrame() == -1 || !SaveState(LastSavedFrame() + 1)) {
		return false;
	}
	auto [len, buffer] = buffers[frame];
	verify(buffer != nullptr);
	rend_start_rollback();
	Deserializer deser(buffer, len, true);
	int frame_;
	deser >> frame_;
	verify(frame == frame_);
	memwatch::unprotect();

	for (auto rit = deltaStates.rbegin(); rit != deltaStates.rend() && rit->first >= frame;) {
		const MemPages& pages = rit->second;
		for (const auto& pair : pages.ram) memcpy(memwatch::ramWatcher.getMemPage(pair.first), &pair.second.data[0], PAGE_SIZE);
		for (const auto& pair : pages.vram) memcpy(memwatch::vramWatcher.getMemPage(pair.first), &pair.second.data[0], PAGE_SIZE);
		for (const auto& pair : pages.aram) memcpy(memwatch::aramWatcher.getMemPage(pair.first), &pair.second.data[0], PAGE_SIZE);
		for (const auto& pair : pages.elanram) memcpy(memwatch::elanWatcher.getMemPage(pair.first), &pair.second.data[0], PAGE_SIZE);
		deltaStates.erase(--rit.base());
	}

	for (auto rit = buffers.rbegin(); rit != buffers.rend() && rit->first > frame;) {
		free(rit->second.second);
		buffers.erase(--rit.base());
	}

	dc_deserialize(deser);
	if (deser.size() != (u32)len) {
		ERROR_LOG(NETWORK, "load_game_state len %d used %d", len, (int)deser.size());
		die("fatal");
	}
	rend_allow_rollback();	// ggpo might load another state right after this one
	memwatch::reset();
	memwatch::protect();
	gdxsv_camera_state_loaded();
	return true;
}

void GdxsvSaveState::Clear() {
	const bool has_save_state = !buffers.empty();
	for (const auto& buffer : buffers) {
		free(buffer.second.second);
	}
	buffers.clear();
	deltaStates.clear();
	if (has_save_state) {
		memwatch::unprotect();
	}
	memwatch::reset();
}

void GdxsvSaveState::Reset() {
	Clear();
	enabled = false;
}

GdxsvSaveState gdxsv_save_state;
