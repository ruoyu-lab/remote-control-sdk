// A viewer's input, executed in this application (RCS_INPUT_HOST): where the
// session shares the application's own picture (iOS lets no application
// control others), its pointer, wheel and keys become Qt's input events in
// the window, and its text goes to the focused field.
#pragma once

#include <rcsdk/rcsdk.h>

#include <QPointer>
#include <QWindow>

class AppInput {
public:
    void setWindow(QWindow* window) { window_ = window; }
    void execute(const rcs_input_event& event);

private:
    void key(uint32_t usage, bool down, bool repeat);
    void releaseAll();

    QPointer<QWindow> window_;
    Qt::MouseButtons buttons_;
    QPointF position_; // Window coordinates of the last pointer event.
    bool shift_ = false;
};
