// iOS: a ProMotion display runs at 60 Hz until something asks for more, and
// Qt Quick does not; while the demo shows a stream it asks for the display's
// highest rate, so that pictures coming at up to 120 a second are shown as
// they come.
#pragma once

class DisplayRate {
public:
    DisplayRate() = default;
    ~DisplayRate();
    DisplayRate(const DisplayRate&) = delete;
    DisplayRate& operator=(const DisplayRate&) = delete;

    void setHighest(bool highest);

private:
    void* link_ = nullptr; // CADisplayLink, retained.
};
