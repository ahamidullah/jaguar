#include "../Time.h"
#include "../Log.h"

#define SecondsToNanoseconds(s) (s * 1000000000)
#define NanosecondsToMilliseconds(n) (n / 1000000)
#define NanosecondsToSeconds(n) (NanosecondsToMilliseconds(n) / 1000)
#define NanosecondsToMinutes(n) (NanosecondsToSeconds(n) / 60)
#define NanosecondsToHours(n) (NanosecondsToMinutes(n) / 60)

bool Duration::operator>(Duration d)
{
	return this->nanoseconds > d.nanoseconds;
}

bool Duration::operator<(Duration d)
{
	return this->nanoseconds < d.nanoseconds;
}

Duration Duration::operator-(Duration d)
{
	return
	{
		.nanoseconds = this->nanoseconds - d.nanoseconds,
	};
}

s64 Duration::Hour()
{
	return NanosecondsToHours(this->nanoseconds);
}

s64 Duration::Minute()
{
	return NanosecondsToMinutes(this->nanoseconds);
}

s64 Duration::Second()
{
	return NanosecondsToSeconds(this->nanoseconds);
}

s64 Duration::Millisecond()
{
	return NanosecondsToMilliseconds(this->nanoseconds);
}

s64 Duration::Nanosecond()
{
	return this->nanoseconds;
}

bool PlatformTime::operator>(PlatformTime t)
{
	if (this->ts.tv_sec > t.ts.tv_sec)
	{
		return true;
	}
	else if (t.ts.tv_sec > this->ts.tv_sec)
	{
		return false;
	}
	else if (this->ts.tv_nsec > t.ts.tv_nsec)
	{
		return true;
	}
	else if (t.ts.tv_nsec > this->ts.tv_nsec)
	{
		return false;
	}
	return false;
}

bool PlatformTime::operator<(PlatformTime t)
{
	if (this->ts.tv_sec < t.ts.tv_sec)
	{
		return true;
	}
	else if (t.ts.tv_sec < this->ts.tv_sec)
	{
		return false;
	}
	else if (this->ts.tv_nsec < t.ts.tv_nsec)
	{
		return true;
	}
	else if (t.ts.tv_nsec < this->ts.tv_nsec)
	{
		return false;
	}
	return false;
}

Duration PlatformTime::operator-(PlatformTime t)
{
	return
	{
		.nanoseconds = (SecondsToNanoseconds(t.ts.tv_sec - this->ts.tv_sec)) + (t.ts.tv_nsec - this->ts.tv_nsec),
	};
}

Date PlatformTime::Date()
{
	auto tm = localtime(&this->ts.tv_sec);
	auto ms = this->ts.tv_nsec / 1000000;
	auto ns = this->ts.tv_nsec - (ms * 1000000);
	return
	{
		tm->tm_year,
		tm->tm_mon,
		tm->tm_mday,
		tm->tm_hour,
		tm->tm_min,
		tm->tm_sec,
		ms,
		ns,
	};
}

s64 PlatformTime::Year()
{
	auto tm = localtime(&this->ts.tv_sec);
	return tm->tm_year;
}

s64 PlatformTime::Month()
{
	auto tm = localtime(&this->ts.tv_sec);
	return tm->tm_mon;
}

s64 PlatformTime::Day()
{
	auto tm = localtime(&this->ts.tv_sec);
	return tm->tm_mday;
}

s64 PlatformTime::Hour()
{
	auto tm = localtime(&this->ts.tv_sec);
	return tm->tm_hour;
}

s64 PlatformTime::Minute()
{
	auto tm = localtime(&this->ts.tv_sec);
	return tm->tm_min;
}

s64 PlatformTime::Second()
{
	auto tm = localtime(&this->ts.tv_sec);
	return tm->tm_sec;
}

s64 PlatformTime::Millisecond()
{
	auto tm = localtime(&this->ts.tv_sec);
	return this->ts.tv_nsec / 1000000;
}

s64 PlatformTime::Nanosecond()
{
	auto tm = localtime(&this->ts.tv_sec);
	auto ms = this->ts.tv_nsec / 1000000;
	return this->ts.tv_nsec - (ms * 1000000);
}

PlatformTime XCurrentTime()
{
	auto t = PlatformTime{};
	clock_gettime(CLOCK_MONOTONIC_RAW, &t.ts);
	return t;
}

void Sleep(s64 msec)
{
	auto ts = (struct timespec)
	{
		.tv_sec = msec / 1000,
		.tv_nsec = (msec % 1000) * 1000000,
	};
	if (nanosleep(&ts, NULL))
	{
		LogError("Time", "nanosleep() ended early: %k.\n", PlatformError());
	}
}
