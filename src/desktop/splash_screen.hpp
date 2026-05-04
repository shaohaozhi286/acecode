#pragma once

namespace acecode::desktop {

class SplashScreen {
public:
    SplashScreen() = default;
    ~SplashScreen();

    SplashScreen(const SplashScreen&) = delete;
    SplashScreen& operator=(const SplashScreen&) = delete;

    void show();
    void close();

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace acecode::desktop
