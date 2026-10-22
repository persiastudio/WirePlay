// Launches the GPL engine as a child process with binary pipes.
//
// The engine is a separate process on purpose: pymobiledevice3 is GPL-3.0 and
// this application is MIT, so the licence boundary has to be a process edge.
// See docs/LEGAL.md.
#pragma once

#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#endif

namespace wp {

class EngineProcess {
public:
    EngineProcess() = default;
    ~EngineProcess();
    EngineProcess(const EngineProcess&) = delete;
    EngineProcess& operator=(const EngineProcess&) = delete;

    // argv[0] is the program; the rest are arguments. `extra` are forwarded
    // verbatim, which is how --ltrp / --no-audio / --udid reach the engine.
    bool start(const std::vector<std::string>& argv);

    // True when the child is gone, with its exit status.
    bool exited(int* status);

    void stop();

    FILE* toEngine() const { return in_; }    // engine's stdin  (our writes)
    FILE* fromEngine() const { return out_; } // engine's stdout (our reads)

    const std::string& error() const { return error_; }

private:
    void closeAll();

    FILE* in_ = nullptr;
    FILE* out_ = nullptr;
    std::string error_;
#ifdef _WIN32
    PROCESS_INFORMATION pi_ {};
    bool have_pi_ = false;
    // Job object the engine runs inside; see start() for why it exists.
    HANDLE job_ = nullptr;
#else
    pid_t pid_ = -1;
#endif
};

// Splits a command line on whitespace, honouring double quotes, so
// --engine "C:\Program Files\x\python.exe" script.py works.
std::vector<std::string> splitCommandLine(const std::string& cmd);

}  // namespace wp
