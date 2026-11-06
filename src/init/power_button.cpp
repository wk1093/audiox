#include "hardware_power_button.hpp"

#include <fcntl.h>
#include <errno.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

namespace {

uint64_t monotonicMs() {
	struct timespec ts{};
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
	return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

template <size_t N>
bool bitSet(const unsigned long (&bits)[N], unsigned int bit) {
	constexpr unsigned int wordBits = sizeof(unsigned long) * 8U;
	const unsigned int word = bit / wordBits;
	return word < N && (bits[word] & (1UL << (bit % wordBits))) != 0;
}

bool hasPowerKey(int fd) {
	unsigned long eventBits[(EV_MAX + sizeof(unsigned long) * 8U) /
							(sizeof(unsigned long) * 8U)]{};
	if (ioctl(fd, EVIOCGBIT(0, sizeof(eventBits)), eventBits) < 0 ||
		!bitSet(eventBits, EV_KEY)) {
		return false;
	}
	unsigned long keyBits[(KEY_MAX + sizeof(unsigned long) * 8U) /
						  (sizeof(unsigned long) * 8U)]{};
	return ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits) >= 0 &&
		bitSet(keyBits, KEY_POWER);
}

bool isOnboardPowerButton(int fd) {
	input_id id{};
	if (ioctl(fd, EVIOCGID, &id) < 0 || id.bustype != BUS_HOST) return false;
	char name[256]{};
	if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) return false;
	name[sizeof(name) - 1] = '\0';
	return strcmp(name, "pwr_button") == 0;
}

} // namespace

PowerButton::PowerButton(bool enabled) : enabled_(enabled) {
	for (int &fd : eventFds_) fd = -1;
}

PowerButton::~PowerButton() {
	for (int &fd : eventFds_) {
		if (fd >= 0) close(fd);
		fd = -1;
	}
}

bool PowerButton::isPowerButtonPress(const input_event &event) {
	return event.type == EV_KEY && event.code == KEY_POWER && event.value == 1;
}

void PowerButton::scanDevices(uint64_t nowMs) {
	nextScanMs_ = nowMs + 1000;
	for (int index = 0; index < kMaxDevices; ++index) {
		if (eventFds_[index] >= 0) continue;
		char path[64];
		snprintf(path, sizeof(path), "/dev/input/event%d", index);
		const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) continue;
		if (hasPowerKey(fd) && isOnboardPowerButton(fd)) {
			eventFds_[index] = fd;
		} else {
			close(fd);
		}
	}
}

bool PowerButton::pollPressed() {
	if (!enabled_) return false;
	const uint64_t nowMs = monotonicMs();
	if (nowMs >= nextScanMs_) scanDevices(nowMs);
	bool pressed = false;
	for (int &fd : eventFds_) {
		if (fd < 0) continue;
		input_event events[16]{};
		for (;;) {
			const ssize_t count = read(fd, events, sizeof(events));
			if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
			if (count < 0 && errno == EINTR) continue;
			if (count <= 0 || count % (ssize_t)sizeof(input_event) != 0) {
				close(fd);
				fd = -1;
				break;
			}
			const size_t eventCount = (size_t)count / sizeof(input_event);
			for (size_t i = 0; i < eventCount; ++i) {
				if (isPowerButtonPress(events[i])) pressed = true;
			}
		}
	}
	return pressed;
}
