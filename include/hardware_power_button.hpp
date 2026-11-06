#pragma once

#include <cstdint>
#include <linux/input.h>

class PowerButton {
public:
	explicit PowerButton(bool enabled);
	~PowerButton();
	PowerButton(const PowerButton &) = delete;
	PowerButton &operator=(const PowerButton &) = delete;

	bool pollPressed();
	static bool isPowerButtonPress(const input_event &event);

private:
	static constexpr int kMaxDevices = 64;
	int eventFds_[kMaxDevices]{};
	uint64_t nextScanMs_{0};
	bool enabled_{false};

	void scanDevices(uint64_t nowMs);
};
