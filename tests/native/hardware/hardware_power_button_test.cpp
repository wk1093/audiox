#include "hardware_power_button.hpp"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

namespace {

uint64_t clockMs = 1000;
unsigned openAttempts = 0;
unsigned closeCount = 0;
input_event queuedEvents[4]{};
size_t queuedCount = 0;
size_t queuedOffset = 0;
unsigned usbCloseCount = 0;

void setBit(unsigned long *bits, unsigned int bit) {
	constexpr unsigned int wordBits = sizeof(unsigned long) * 8U;
	bits[bit / wordBits] |= 1UL << (bit % wordBits);
}

int fakeOpen(const char *path, int) {
	++openAttempts;
	if (strcmp(path, "/dev/input/event0") == 0) return 100;
	if (strcmp(path, "/dev/input/event1") == 0) return 101;
	errno = ENOENT;
	return -1;
}

int fakeClose(int fd) {
	if (fd == 100) {
		++usbCloseCount;
		return 0;
	}
	assert(fd == 101);
	++closeCount;
	return 0;
}

int fakeIoctl(int fd, unsigned long request, void *data) {
	assert(fd == 100 || fd == 101);
	memset(data, 0, _IOC_SIZE(request));
	if (request == EVIOCGID) {
		input_id *id = static_cast<input_id *>(data);
		id->bustype = fd == 100 ? BUS_USB : BUS_HOST;
		return 0;
	}
	if (_IOC_NR(request) == _IOC_NR(EVIOCGNAME(0))) {
		snprintf(static_cast<char *>(data), _IOC_SIZE(request), "%s",
				 fd == 100 ? "USB Keyboard" : "pwr_button");
		return 0;
	}
	unsigned long *bits = static_cast<unsigned long *>(data);
	if (_IOC_NR(request) == _IOC_NR(EVIOCGBIT(0, 0))) {
		setBit(bits, EV_KEY);
		return 0;
	}
	if (_IOC_NR(request) == _IOC_NR(EVIOCGBIT(EV_KEY, 0))) {
		setBit(bits, KEY_POWER);
		return 0;
	}
	errno = EINVAL;
	return -1;
}

ssize_t fakeRead(int fd, void *buffer, size_t capacity) {
	assert(fd == 101);
	if (queuedOffset == queuedCount) {
		errno = EAGAIN;
		return -1;
	}
	assert(capacity >= sizeof(input_event));
	memcpy(buffer, &queuedEvents[queuedOffset++], sizeof(input_event));
	return sizeof(input_event);
}

int fakeClock(clockid_t, timespec *ts) {
	ts->tv_sec = (time_t)(clockMs / 1000);
	ts->tv_nsec = (long)(clockMs % 1000) * 1000000L;
	return 0;
}

void queueKey(int code, int value) {
	assert(queuedCount < sizeof(queuedEvents) / sizeof(queuedEvents[0]));
	queuedEvents[queuedCount++] = {};
	queuedEvents[queuedCount - 1].type = EV_KEY;
	queuedEvents[queuedCount - 1].code = (uint16_t)code;
	queuedEvents[queuedCount - 1].value = value;
}

} // namespace

#define open(...) fakeOpen(__VA_ARGS__)
#define close(...) fakeClose(__VA_ARGS__)
#define ioctl(...) fakeIoctl(__VA_ARGS__)
#define read(...) fakeRead(__VA_ARGS__)
#define clock_gettime(...) fakeClock(__VA_ARGS__)
#include "../../../src/init/power_button.cpp"
#undef open
#undef close
#undef ioctl
#undef read
#undef clock_gettime

int main() {
	assert(!isOnboardPowerButton(100));
	assert(isOnboardPowerButton(101));
	{
		PowerButton button(true);
		assert(!button.pollPressed());
		assert(openAttempts == 64);
		assert(usbCloseCount == 1);

		// The USB keyboard also advertises KEY_POWER, but scanning retains only pwr_button.
		queueKey(KEY_POWER, 1);
		assert(button.pollPressed());
		queueKey(KEY_POWER, 2);
		queueKey(KEY_POWER, 0);
		assert(!button.pollPressed());
		queueKey(KEY_SLEEP, 1);
		assert(!button.pollPressed());

		clockMs += 1000;
		assert(!button.pollPressed());
		assert(openAttempts == 127);
		assert(closeCount == 0);
	}
	assert(closeCount == 1);
}
