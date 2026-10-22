#include "wp/process.hpp"

#include <fcntl.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace wp {

std::vector<std::string> splitCommandLine(const std::string& cmd) {
    std::vector<std::string> out;
    std::string cur;
    bool in_quotes = false;
    bool has_token = false;
    for (char c : cmd) {
        if (c == '"') {
            in_quotes = !in_quotes;
            has_token = true;
        } else if (!in_quotes && (c == ' ' || c == '\t')) {
            if (has_token) { out.push_back(cur); cur.clear(); has_token = false; }
        } else {
            cur.push_back(c);
            has_token = true;
        }
    }
    if (has_token) out.push_back(cur);
    return out;
}

EngineProcess::~EngineProcess() {
    stop();
    closeAll();
#ifdef _WIN32
    // stop() has already terminated the child, so this normally just frees the
    // object. If anything inside the job somehow survived, KILL_ON_JOB_CLOSE
    // ends it here.
    if (job_) { CloseHandle(job_); job_ = nullptr; }
#endif
}

void EngineProcess::closeAll() {
    if (in_) { std::fclose(in_); in_ = nullptr; }
    if (out_) { std::fclose(out_); out_ = nullptr; }
}

#ifdef _WIN32

namespace {

// An anonymous job object that terminates every process inside it as soon as
// the last handle closes. This is what ties the engine's lifetime to ours:
// Windows performs the kill on process teardown, so it happens even when
// wireplay.exe is ended from Task Manager or dies mid-crash, neither of which
// runs a destructor.
//
// Without it, killing this app orphans python.exe. The orphan keeps running and
// keeps whatever directory it was started in as its working directory --
// normally the project folder -- and a working directory is enough for Windows
// to refuse to delete that folder while naming no process at all.
//
// Returns nullptr on failure; the caller runs without the guarantee rather than
// refusing to start the engine.
HANDLE makeKillOnCloseJob() {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info {};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
        CloseHandle(job);
        return nullptr;
    }
    return job;
}

}  // namespace

bool EngineProcess::start(const std::vector<std::string>& argv) {
    SECURITY_ATTRIBUTES sa {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE to_child_r = nullptr, to_child_w = nullptr;
    HANDLE from_child_r = nullptr, from_child_w = nullptr;
    if (!CreatePipe(&to_child_r, &to_child_w, &sa, 0) ||
        !CreatePipe(&from_child_r, &from_child_w, &sa, 0)) {
        error_ = "CreatePipe failed";
        return false;
    }
    // The parent's ends must not be inheritable, or the child inherits a
    // reference to them and closing our handle never produces EOF.
    SetHandleInformation(to_child_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(from_child_r, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = to_child_r;
    si.hStdOutput = from_child_w;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);  // engine logs stay visible

    std::string cmd;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) cmd.push_back(' ');
        const bool needs_quotes = argv[i].find(' ') != std::string::npos;
        if (needs_quotes) cmd.push_back('"');
        cmd += argv[i];
        if (needs_quotes) cmd.push_back('"');
    }

    if (job_) { CloseHandle(job_); job_ = nullptr; }  // a second start() must not leak one
    HANDLE job = makeKillOnCloseJob();

    // CREATE_SUSPENDED when the job exists: the child must be inside the job
    // before it runs, or it could spawn a child of its own that escapes it.
    const BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                   job ? CREATE_SUSPENDED : 0, nullptr, nullptr, &si, &pi_);
    CloseHandle(to_child_r);
    CloseHandle(from_child_w);
    if (!ok) {
        error_ = "CreateProcess failed for: " + cmd;
        CloseHandle(to_child_w);
        CloseHandle(from_child_r);
        if (job) CloseHandle(job);
        return false;
    }
    if (job) {
        if (!AssignProcessToJobObject(job, pi_.hProcess)) {
            // Not fatal, but the safety net is gone: say so rather than let an
            // orphan turn up with no explanation.
            std::fprintf(stderr,
                         "[wireplay] warning: could not put the engine in a job object "
                         "(win32 error %lu); if this app is killed, end the engine by hand\n",
                         GetLastError());
        }
        ResumeThread(pi_.hThread);  // needed even when the assign failed
        job_ = job;
    }
    have_pi_ = true;

    const int fd_in = _open_osfhandle(reinterpret_cast<intptr_t>(to_child_w), 0);
    const int fd_out = _open_osfhandle(reinterpret_cast<intptr_t>(from_child_r), _O_RDONLY);
    if (fd_in < 0 || fd_out < 0) { error_ = "_open_osfhandle failed"; return false; }
    in_ = _fdopen(fd_in, "wb");
    out_ = _fdopen(fd_out, "rb");
    if (!in_ || !out_) { error_ = "_fdopen failed"; return false; }
    _setmode(_fileno(in_), _O_BINARY);
    _setmode(_fileno(out_), _O_BINARY);
    return true;
}

bool EngineProcess::exited(int* status) {
    if (!have_pi_) return true;
    if (WaitForSingleObject(pi_.hProcess, 0) != WAIT_OBJECT_0) return false;
    DWORD code = 0;
    GetExitCodeProcess(pi_.hProcess, &code);
    if (status) *status = int(code);
    return true;
}

void EngineProcess::stop() {
    if (in_) { std::fclose(in_); in_ = nullptr; }  // EOF on stdin = clean exit request
    if (!have_pi_) return;
    // 6 s, not 3: a QuickTime teardown verifies by re-enumeration that the
    // phone really dropped its capture configuration, and that cannot be
    // rushed without giving up the verification.
    if (WaitForSingleObject(pi_.hProcess, 6000) != WAIT_OBJECT_0) {
        TerminateProcess(pi_.hProcess, 1);
        WaitForSingleObject(pi_.hProcess, 2000);
    }
    CloseHandle(pi_.hThread);
    CloseHandle(pi_.hProcess);
    std::memset(&pi_, 0, sizeof(pi_));
    have_pi_ = false;
}

#else  // POSIX

bool EngineProcess::start(const std::vector<std::string>& argv) {
    if (argv.empty()) { error_ = "empty command"; return false; }

    int to_child[2], from_child[2];
    if (pipe(to_child) != 0 || pipe(from_child) != 0) {
        error_ = std::string("pipe failed: ") + std::strerror(errno);
        return false;
    }

    pid_ = fork();
    if (pid_ < 0) {
        error_ = std::string("fork failed: ") + std::strerror(errno);
        return false;
    }
    if (pid_ == 0) {
        // Child: wire the pipes onto stdin/stdout, keep stderr for logs.
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);

        std::vector<char*> c_argv;
        c_argv.reserve(argv.size() + 1);
        for (const auto& s : argv) c_argv.push_back(const_cast<char*>(s.c_str()));
        c_argv.push_back(nullptr);
        execvp(c_argv[0], c_argv.data());
        std::fprintf(stderr, "[wireplay] exec failed for %s: %s\n", c_argv[0], std::strerror(errno));
        _exit(127);
    }

    close(to_child[0]);
    close(from_child[1]);
    in_ = fdopen(to_child[1], "wb");
    out_ = fdopen(from_child[0], "rb");
    if (!in_ || !out_) { error_ = "fdopen failed"; return false; }
    return true;
}

bool EngineProcess::exited(int* status) {
    if (pid_ < 0) return true;
    int st = 0;
    const pid_t r = waitpid(pid_, &st, WNOHANG);
    if (r == 0) return false;
    if (status) *status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    pid_ = -1;
    return true;
}

void EngineProcess::stop() {
    if (in_) { std::fclose(in_); in_ = nullptr; }  // kEnd arrives as EOF
    if (pid_ < 0) return;
    kill(pid_, SIGTERM);
    for (int i = 0; i < 30; ++i) {
        int st = 0;
        if (waitpid(pid_, &st, WNOHANG) != 0) { pid_ = -1; return; }
        struct timespec ts {0, 100 * 1000 * 1000};
        nanosleep(&ts, nullptr);
    }
    kill(pid_, SIGKILL);
    int st = 0;
    waitpid(pid_, &st, 0);
    pid_ = -1;
}

#endif

}  // namespace wp
