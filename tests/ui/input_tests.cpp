#include "paper/input.hpp"
#include "paper/ui/paint.hpp"
#include <iostream>
int main() {
    paper::Input frame;
    frame.x = 20;
    frame.y = 30;
    paper::InputSample down;
    down.x = 10;
    down.y = 15;
    down.click = true;
    frame.events.push_back(down);
    auto up = down;
    up.click = false;
    up.released = true;
    frame.events.push_back(up);
    auto wheel = up;
    wheel.released = false;
    wheel.wheelY = 2;
    frame.events.push_back(wheel);
    auto key = wheel;
    key.wheelY = 0;
    key.repeatedKey = SDLK_TAB;
    key.held[SDL_SCANCODE_LSHIFT] = true;
    frame.events.push_back(key);
    const auto events = paper::ui::engineInput(frame, 30);
    bool ok = events.size() == 5 && events[0].type == paper::ui::InputType::Down &&
              events[1].type == paper::ui::InputType::Up && events[2].wheel == -60 &&
              events[3].key == paper::ui::Key::Tab && events[3].repeat && events[3].shift &&
              events[4].position.x == 20;
    frame.focusLost = true;
    const auto lost = paper::ui::engineInput(frame);
    ok &= lost.size() == 1 && lost[0].type == paper::ui::InputType::FocusLost;
    frame.focusLost = false;
    frame.debugCaptured = true;
    ok &= paper::ui::engineInput(frame).size() == 1;
    std::cout << "UI input contract " << (ok ? "passed" : "failed") << '\n';
    return ok ? 0 : 1;
}
