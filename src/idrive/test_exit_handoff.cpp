// Exercise the plugin's actual poller and checker with a private fake RAM filesystem and HMI.
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <spawn.h>
#include <signal.h>
#include <pthread.h>
#include <new>
#include <stdarg.h>
#include <stdint.h>
#include <string>
#include <vector>

#define NBT_IMAGE_ABI_HPP
#define NBT_OPEN_TOOLCHAIN
#define NBT_HOST_TEST
#define NBT_QNX_SPAWN_COMPAT_H
#define SPAWN_SETGROUP 1
#define SPAWN_SETSIGDEF 2
#define SPAWN_SETSIGMASK 4
#define SPAWN_NOZOMBIE 8
#define SPAWN_NEWPGROUP 0
struct inheritance { int flags; pid_t pgroup; sigset_t sigmask, sigdefault; };
static pid_t spawn(const char*, int, const int*, const struct inheritance*, char* const[], char* const[])
{
    abort();
}
namespace bmw {
template<class T> class BasicString {
public:
    explicit BasicString(const T* p) : data_(p) {}
    BasicString(const BasicString& other) : data_(other.data_) {}
private:
    const T* data_;
};
class Runnable {
public:
    virtual ~Runnable() {}
    virtual void run() = 0;
};
}

static std::string test_dir;
static std::string test_path(const char* path)
{
    const char prefix[] = "/fs/ram3/";
    return strncmp(path, prefix, sizeof(prefix)-1) == 0 ? test_dir + "/" + (path + sizeof(prefix)-1) : path;
}
static int test_open(const char* path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    return open(test_path(path).c_str(), flags, mode);
}
static int test_unlink(const char* path) { return unlink(test_path(path).c_str()); }
static int test_access(const char* path, int mode) { return access(test_path(path).c_str(), mode); }
static int test_stat(const char* path, struct stat* st) { return stat(test_path(path).c_str(), st); }
#define open test_open
#define unlink test_unlink
#define access test_access
#define stat(path, st) test_stat(path, st)
#include "aa_entry_plugin.cpp"
#undef open
#undef unlink
#undef access
#undef stat

static std::vector<unsigned> events;
namespace bmw {
class IDispatcher {};
class Bundle {};
PluginApp::PluginApp(IDispatcher&, BasicString<char>, const Bundle&, const Bundle&) {}
PluginApp::~PluginApp() {}
void PluginApp::post(Runnable*, unsigned) {}
void PluginApp::triggerEvent(unsigned event) { events.push_back(event); }
void PluginApp::updateModel(unsigned, int) {}
void PluginApp::updateModel(unsigned, const TextID&) {}
void PluginApp::cdsAddPropertyChangedEventHandler(BasicString<char>&, unsigned, const CdsDelegate&) { abort(); }
void PluginApp::addHandler(unsigned, unsigned, const ActionDelegate&) { abort(); }
bool PluginApp::start() { abort(); }
bool Parcel::readBool(bool&) const { abort(); }
TextID::TextID(unsigned) {}
}

static void require(bool condition, const char* message)
{
    if (!condition) {
        fprintf(stderr, "iDrive handoff: %s\n", message);
        exit(1);
    }
}
static void write_file(const char* path, const char* text)
{
    int fd = test_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    require(fd >= 0, "create fixture");
    const size_t size = strlen(text);
    require(write(fd, text, size) == (ssize_t)size, "write fixture");
    close(fd);
}
static bool exists(const char* path) { return test_access(path, F_OK) == 0; }
static void request_exit(long long deadline)
{
    char text[40];
    snprintf(text, sizeof(text), "%lld\n", deadline);
    write_file(kExitToIdrive, text);
}

int main(int argc, char** argv)
{
    require(argc == 2, "expected private fixture directory");
    test_dir = argv[1];
    write_file(kState, "state=running phone=1\n");
    bmw::IDispatcher dispatcher;
    bmw::Bundle bundle;
    bmw::PluginApp app(dispatcher, bmw::BasicString<char>("test"), bundle, bundle);
    Entry e;
    e.app = &app;
    e.target.app = &app;
    e.focusMask = 1;
    e.showUntilMs = now_ms() + 4000;
    e.showRetryAtMs = now_ms() + 1000;
    g_pdcFromAa = true;
    write_file(kShowRequest, "");
    write_file(kBackKey, "");
    request_exit(now_ms() - 1);
    e.target.run();
    require(events.empty() && !e.exitPending, "ignore stale exit request");
    request_exit(now_ms() + 4000);
    e.target.run();
    require(events.size() == 1 && events[0] == 720 && e.exitPending, "request main menu");
    require(!exists(kShowRequest) && !exists(kBackKey) && exists(kLeave) && !g_pdcFromAa &&
            !e.showUntilMs && !e.showRetryAtMs, "cancel stale return requests");
    e.exitRetryMs = now_ms() - 1;
    e.target.run();
    require(events.size() == 2 && events[1] == 721, "retry with a different event");
    e.focusMask = 0;
    e.checker.run();
    require(!e.exitPending && exists(kHidden) && !exists(kLeave), "hide only after HMI focus leaves");

    test_unlink(kHidden);
    e.focusMask = 1;
    request_exit(now_ms() + 4000);
    e.target.run();
    require(e.exitPending && events.back() == 722, "a second exit uses the next event");
    e.exitDeadlineMs = now_ms() - 1;
    e.target.run();
    require(!e.exitPending && !exists(kLeave) && !exists(kHidden), "failed handoff does not hide iDrive");

    request_exit(now_ms() + 4000);
    e.target.run();
    write_file(kShowRequest, "");
    e.target.run();
    require(!e.exitPending && !exists(kLeave), "explicit show cancels pending exit");
    puts("iDrive exit handoff OK");
    return 0;
}
