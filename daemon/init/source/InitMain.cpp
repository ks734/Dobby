/*
* If not stated otherwise in this file or this component's LICENSE file the
* following copyright and licenses apply:
*
* Copyright 2016 Sky UK
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/
/*
 *  File:   InitMain.cpp
 *  Author:
 *
 *
 *  This file creates a very simple 'init' process for the container.  The main
 *  motivation for this is described here:
 *
 *  https://blog.phusion.nl/2015/01/20/docker-and-the-pid-1-zombie-reaping-problem/
 *
 *  It boils down to ensuring we have an 'init' process that does at least the
 *  following things:
 *
 *      1. Reaps adopted child processes.
 *      2. Forwards on signals to child processes.
 *      3. Propagates fatal crash signals (SIGABRT, SIGSEGV, SIGILL, SIGFPE,
 *         SIGQUIT, SIGBUS) as WIFSIGNALED to the parent (Dobby) so that crash
 *         vs clean-stop can be distinguished by callers.  This is done by
 *         re-raising the signal with default disposition after all children
 *         have been reaped.  Graceful-stop signals (SIGTERM, SIGHUP, SIGINT)
 *         are intentionally excluded so that normal Dobby-initiated shutdown
 *         still produces WIFEXITED=true as callers expect.
 *
 *  In addition to the above it provides some basic logging to indicate why a
 *  child process died.
 *
 *  It's worth pointing out that runC does implement a sub-reaper which
 *  is enabled by default - it can be disabled by specifying the
 *  '--no-subreaper' option on the start command line.  However it doesn't
 *  solve the signal problems, and I've found without this code in place the
 *  only way to kill a process inside a container is with SIGKILL, which is
 *  a bit anti-social.
 *
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/resource.h>

#include <vector>
#include <string>
#include <cstdlib>

#ifndef TEMP_FAILURE_RETRY
#define TEMP_FAILURE_RETRY(exp)            \
  ({                                       \
    decltype(exp) _rc;                     \
    do {                                   \
      _rc = (exp);                         \
    } while (_rc == -1 && errno == EINTR); \
    _rc;                                   \
  })
#endif

#if defined(USE_ETHANLOG)

    #include <ethanlog.h>

    #define LOG_ERR(fmt,...) \
        do { \
            ethanlog(ETHAN_LOG_ERROR, __FILE__, __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__); \
            fprintf(stderr, fmt "\n", ##__VA_ARGS__); \
        } while(0)

    #define LOG_NFO(fmt,...) \
        do { \
            ethanlog(ETHAN_LOG_INFO, __FILE__, __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__); \
            fprintf(stdout, fmt "\n", ##__VA_ARGS__); \
        } while(0)

#else

    #define LOG_ERR(fmt,...) \
        do { \
            fprintf(stderr, fmt "\n", ##__VA_ARGS__); \
        } while(0)

    #define LOG_NFO(fmt,...) \
        do { \
            fprintf(stdout, fmt "\n", ##__VA_ARGS__); \
        } while(0)

#endif



static void closeAllFileDescriptors(int logPipeFd)
{
    // the two options for this are to loop over every possible file descriptor
    // (usually 1024), or read /proc/self/fd/ directory.  I've gone for the
    // later as think it's slightly nicer although more cumbersome to implement.

    // get the fd rlimit
    struct rlimit rlim;
    if (getrlimit(RLIMIT_NOFILE, &rlim) != 0)
    {
        LOG_ERR("failed to get the fd rlimit, defaulting to 1024 (%d - %s)",
                errno, strerror(errno));
        rlim.rlim_cur = 1024;
    }

    // iterate through all the fd sym links
    int dirFd = open("/proc/self/fd/", O_DIRECTORY | O_CLOEXEC);
    if (dirFd < 0)
    {
        LOG_ERR("failed to open '/proc/self/fd/' directory (%d - %s)",
                errno, strerror(errno));
        return;
    }

    DIR *dir = fdopendir(dirFd);
    if (!dir)
    {
        LOG_ERR("failed to open '/proc/self/fd/' directory");
        return;
    }

    std::vector<int> openFds;
    openFds.reserve(8);

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr)
    {
        if (entry->d_type != DT_LNK)
            continue;

        // get the fd and sanity check it's in the valid range
        long fd = std::strtol(entry->d_name, nullptr, 10);
        if ((fd < 3) || (fd > (long)rlim.rlim_cur))
            continue;

        // skip the fd opened for iterating the directory
        if (fd == dirFd)
            continue;

        openFds.push_back(int(fd));
    }

    closedir(dir);

    // close all the open fds (except stdin, stdout or stderr)
    for (int fd : openFds)
    {
        // don't close the logging pipe
        if ((logPipeFd >= 0) && (fd == logPipeFd))
            continue;

        // close all the other descriptors
        if (close(fd) != 0)
            LOG_ERR("failed to close fd %d (%d - %s)", fd, errno, strerror(errno));
    }
}

#if (AI_BUILD_TYPE == AI_DEBUG)

static bool readCgroup(const std::string &cgroup, unsigned long *val)
{
    static const std::string base = "/sys/fs/cgroup/";
    std::string path = base + cgroup;

    FILE *fp = fopen(path.c_str(), "r");
    if (!fp)
    {
        if (errno != ENOENT)
            LOG_ERR("failed to open '%s' (%d - %s)", path.c_str(), errno, strerror(errno));

        return false;
    }

    char* line = nullptr;
    size_t len = 0;
    ssize_t rd;

    if ((rd = getline(&line, &len, fp)) < 0)
    {
        if (line)
            free(line);
        fclose(fp);
        LOG_ERR("failed to read cgroup file line (%d - %s)", errno, strerror(errno));
        return false;
    }

    *val = strtoul(line, nullptr, 0);

    fclose(fp);
    free(line);

    return true;
}

static void checkForOOM(void)
{
    unsigned long failCnt;

    if (readCgroup("memory/memory.failcnt", &failCnt) && (failCnt > 0))
    {
        LOG_ERR("memory allocation failure detected in container, likely OOM (failcnt = %lu)", failCnt);
    }

    if (readCgroup("gpu/gpu.failcnt", &failCnt) && (failCnt > 0))
    {
        LOG_NFO("GPU memory allocation failure detected in container (failcnt = %lu)", failCnt);
    }
}

#endif // (AI_BUILD_TYPE == AI_DEBUG)

// Fatal signal received by DobbyInit itself (set in signalHandler).
// Only crash/fatal signals are recorded here (SIGABRT, SIGSEGV, SIGILL,
// SIGFPE, SIGQUIT, SIGBUS) — NOT graceful-stop signals such as SIGTERM,
// SIGHUP, SIGINT or app-defined signals (SIGUSR1/2).
// Used to re-raise the signal after all children are reaped so that
// Dobby's waitpid() sees WIFSIGNALED=true / WTERMSIG=<signal> rather
// than WIFEXITED=true.
static volatile sig_atomic_t gReceivedFatalSignal = 0;

// Returns true for signals that represent a crash/abnormal termination and
// should be propagated as WIFSIGNALED to Dobby's waitpid().
// Graceful-stop signals (SIGTERM, SIGHUP, SIGINT, SIGALRM) must NOT be
// included — Dobby sends SIGTERM for normal container shutdown and expects
// WIFEXITED=true in that case.
static bool isFatalSignal(int sigNum)
{
    switch (sigNum)
    {
        case SIGABRT:
        case SIGSEGV:
        case SIGILL:
        case SIGFPE:
        case SIGQUIT:   // produces core dump
#ifdef SIGBUS
        case SIGBUS:
#endif
            return true;
        default:
            return false;
    }
}

static int doForkExec(int argc, char * argv[])
{
    // if a ETHAN_LOG pipe was supplied then we don't want to close that as we
    // use it to log the exit status of the thing we launched
    int logPipeFd = -1;

#if defined(USE_ETHANLOG)
    const char *logPipeEnv = getenv("ETHAN_LOGGING_PIPE");
    if (logPipeEnv)
    {
        long fd = std::strtol(logPipeEnv, nullptr, 10);
        if ((fd >= 3) && (fd <= 1024))
            logPipeFd = static_cast<int>(fd);
    }
#endif

    const int maxArgs = 64;

    if ((argc < 2) || (argc > maxArgs))
    {
        LOG_ERR("to many or too few args (%d)", argc);
        return EXIT_FAILURE;
    }

    pid_t exePid = fork();
    if (exePid < 0)
    {
        LOG_ERR("failed to fork and launch app (%d - %s)", errno, strerror(errno));
        return EXIT_FAILURE;

    }

    if (exePid == 0)
    {
        // the args supplied to the init process are what we supply to the
        // child exec'd process, i.e.
        //
        //   argv[] = { "DobbyInit", <arg1>, <arg2>, ... <argN> }
        //                             /       /           /
        //   args[] = {   basename(<arg1>), <arg2>, ... <argN> }
        //

        char* args[maxArgs];
        char* execBinary = argv[1];

        // the first arg is always the name of the exec being run
        args[0] = basename(execBinary);

        // copy the rest of the args verbatium
        for (int i = 2; i < argc; i++)
            args[i - 1] = argv[i];

        // terminate with a null
        args[(argc - 1)] = nullptr;

        // if the magic env var is set telling us to pause the process for debugging
        // then we raise a SIGSTOP to pause the process.  This is useful for debugging
        // the container startup process, as it allows you to attach a debugger to the
        // process before it starts executing the main process.
        const char *pauseEnv = getenv("DOBBY_INIT_PAUSE");
        if (pauseEnv && (strcmp(pauseEnv, "1") == 0))
        {
            LOG_NFO("pausing the process for debugging");
            raise(SIGSTOP);
        }

        // within forked client so exec the main process
        execvp(argv[1], args);

        // if we reached here then the above has failed
        LOG_ERR("failed exec '%s' (%d - %s)", argv[1], errno, strerror(errno));
        _exit(EXIT_FAILURE);
    }

    // we should now close any file descriptors we have open except for
    // stdin, stdout or stderr.  If we don't do this it's a minor security hole
    // as we'll be holding the file descriptors open for the lifetime of the
    // container ... whereas it's the app that we run that should manage the
    // lifetime of any supplied descriptors (except stdin, stdout and stderr)
    LOG_NFO("forked main child (exePid=%d) for '%s'", exePid, argv[1]);
    closeAllFileDescriptors(logPipeFd);


    int ret = EXIT_FAILURE;

    // Track signals from dying children so DobbyInit can re-raise after all
    // children are reaped. This ensures Dobby's waitpid() on the DobbyInit PID
    // sees WIFSIGNALED=true / WTERMSIG=<signal> rather than WIFEXITED=true.
    //
    // Two cases:
    //  1. exePid itself was killed by a signal (direct child crash).
    //  2. exePid exited cleanly but a sibling child died by signal first
    //     (e.g. exePid is a launcher wrapper whose payload is a grandchild;
    //      the grandchild crashes, its death is forwarded via signalHandler to
    //      all processes including the launcher, which then exits cleanly with
    //      code 0 — DobbyInit only sees the launcher's clean exit).
    //     In this case we use any signal-killed child as the propagated signal.
    int exeTermSignal = 0;   // signal from exePid itself (highest priority)
    int anyTermSignal = 0;   // signal from any other child (fallback)

    // wait for all children to finish
    pid_t pid;
    int status;
    while ((pid = TEMP_FAILURE_RETRY(wait(&status))) != -1)
    {
        if (pid > 0)
        {
            // ###DBG Step4: log raw wait status for each reaped child
            LOG_NFO("###DBG Step4: wait() reaped pid=%d [%s] status=0x%04x "
                    "WIFSIGNALED=%d(sig=%d,core=%d) WIFEXITED=%d(code=%d)",
                    pid, (pid == exePid) ? "exePid" : "child", status,
                    WIFSIGNALED(status), WIFSIGNALED(status) ? WTERMSIG(status) : 0,
                                         WIFSIGNALED(status) ? WCOREDUMP(status) : 0,
                    WIFEXITED(status),   WIFEXITED(status)   ? WEXITSTATUS(status) : -1);

            char msg[128];
            int msglen;

            msglen = snprintf(msg, sizeof(msg), "pid %d [%s] has terminated ",
                           pid, (pid == exePid) ? "exePid" : "child");

            if (WIFSIGNALED(status))
            {
                msglen += snprintf(msg + msglen, sizeof(msg) - msglen,
                                   "by signal %d ", WTERMSIG(status));

                if (WCOREDUMP(status))
                {
                    msglen += snprintf(msg + msglen, sizeof(msg) - msglen,
                                       "and produced a core dump ");
                }

                // record if the main child was killed by a signal so we can
                // propagate it after all children are reaped.
                // Only track fatal/crash signals — not graceful-stop signals.
                if (isFatalSignal(WTERMSIG(status)))
                {
                    if (pid == exePid)
                    {
                        exeTermSignal = WTERMSIG(status);
                        // ###DBG Step4: exePid itself killed by fatal signal
                        // OLD code: ret stays EXIT_FAILURE, DobbyInit returns → WIFEXITED=true (BUG)
                        // NEW code: exeTermSignal=%d recorded, will re-raise after loop
                        LOG_NFO("###DBG Step4: exePid=%d killed by fatal sig=%d "
                                "- OLD: DobbyInit would exit cleanly (WIFEXITED=true BUG); "
                                "NEW: exeTermSignal=%d recorded",
                                pid, exeTermSignal, exeTermSignal);
                    }
                    else if (anyTermSignal == 0)
                    {
                        // keep the first fatal-signal-killed child as fallback
                        anyTermSignal = WTERMSIG(status);
                        LOG_NFO("###DBG Step4: non-exePid child=%d killed by fatal sig=%d "
                                "- anyTermSignal=%d recorded (launcher-wrapper fallback)",
                                pid, anyTermSignal, anyTermSignal);
                    }
                }
                else
                {
                    // non-fatal signal (e.g. SIGTERM) — old and new code both ignore for re-raise
                    LOG_NFO("###DBG Step4: pid=%d killed by non-fatal sig=%d "
                            "- not recorded (graceful-stop signal)",
                            pid, WTERMSIG(status));
                }
            }

            if (WIFEXITED(status))
            {
                msglen += snprintf(msg + msglen, sizeof(msg) - msglen,
                                   "(return code %d)", WEXITSTATUS(status));

                if (pid == exePid)
                {
                    ret = WEXITSTATUS(status);
                    // ###DBG Step5: exePid exited cleanly with code %d
                    // OLD code: DobbyInit would return this → WIFEXITED=true (BUG if crash)
                    LOG_NFO("###DBG Step5: exePid=%d WIFEXITED exitcode=%d - "
                            "OLD code would return this making DobbyDaemon see WIFEXITED=true",
                            pid, ret);
                }
            }

            // if the process died because of a signal, or it didn't exit with
            // success then log as an error, otherwise it's just info
            if (WIFSIGNALED(status) ||
                (WIFEXITED(status) && (WEXITSTATUS(status) != EXIT_SUCCESS)))
            {
                LOG_ERR("%s", msg);
            }
            else
            {
                LOG_NFO("%s", msg);
            }
        }
    }

    // Re-raise signal with default disposition so DobbyInit terminates by it,
    // propagating WIFSIGNALED/WTERMSIG/WCOREDUMP up to Dobby's waitpid().
    //
    // Priority (highest first):
    //  1. gReceivedFatalSignal - DobbyInit itself was sent a fatal signal
    //                            (e.g. kill -11 <pid>). Most authoritative.
    //  2. exeTermSignal        - exePid (main child) was killed by a fatal signal.
    //  3. anyTermSignal        - a sibling child was killed by a fatal signal
    //                            (launcher-wrapper case: exePid exits cleanly
    //                             after its payload crashes).
    //
    // Graceful-stop signals (SIGTERM, SIGHUP, SIGINT, SIGALRM) are excluded
    // from all three sources, so normal Dobby-initiated shutdown continues
    // to produce WIFEXITED=true as callers expect.
    const int sigToRaise = (gReceivedFatalSignal != 0) ? static_cast<int>(gReceivedFatalSignal)
                         : (exeTermSignal        != 0) ? exeTermSignal
                         :                               anyTermSignal;
    // ###DBG Step5+6: summary of what old code would do vs what new code does
    LOG_NFO("###DBG Step5: wait loop done - exePid=%d ret=%d "
            "gReceivedFatalSignal=%d exeTermSignal=%d anyTermSignal=%d sigToRaise=%d",
            exePid, ret, static_cast<int>(gReceivedFatalSignal), exeTermSignal, anyTermSignal, sigToRaise);
    if (sigToRaise == 0)
    {
        LOG_NFO("###DBG Step6(OLD path): sigToRaise=0 → DobbyInit returns ret=%d "
                "→ DobbyDaemon sees WIFEXITED=1 WEXITSTATUS=%d (correct for graceful stop)",
                ret, ret);
    }
    else
    {
        LOG_NFO("###DBG Step6(OLD path would be): ret=%d → DobbyDaemon would see WIFEXITED=1 (BUG)",
                ret);
        LOG_NFO("###DBG Step6(NEW path): re-raising sig=%d with SIG_DFL "
                "→ DobbyInit dies by signal → DobbyDaemon sees WIFSIGNALED=1 WTERMSIG=%d (FIX)",
                sigToRaise, sigToRaise);
    }

    LOG_NFO("wait loop done: exePid=%d ret=%d gReceivedFatalSignal=%d exeTermSignal=%d anyTermSignal=%d sigToRaise=%d",
            exePid, ret, static_cast<int>(gReceivedFatalSignal), exeTermSignal, anyTermSignal, sigToRaise);
    if (sigToRaise != 0)
    {
        LOG_NFO("re-raising signal %d (exePid-signal=%d any-signal=%d) to propagate to Dobby",
                sigToRaise, exeTermSignal, anyTermSignal);
        signal(sigToRaise, SIG_DFL);
        raise(sigToRaise);
        // raise() should not return - if it does fall through to EXIT_FAILURE
    }

#if (AI_BUILD_TYPE == AI_DEBUG)

    // check the memory cgroups memory status for allocation failures, this is
    // an indication of OOMs
    checkForOOM();

#endif

    return ret;
}

static void signalHandler(int sigNum)
{
    // ###DBG Step2: signalHandler called for signal %d (DobbyInit pid=%d)
    // OLD code path: would just call kill(-1, sig) and return — DobbyInit
    // survives the signal and eventually exits cleanly → WIFEXITED=true (BUG)
    // NEW code path: records fatal signals in gReceivedFatalSignal so we can
    // re-raise after children are reaped → WIFSIGNALED=true (FIX)
    LOG_NFO("###DBG Step2: signalHandler(sig=%d) DobbyInit(pid=%d) - "
            "OLD would just forward+return (DobbyInit survives!); "
            "NEW records isFatalSignal=%d",
            sigNum, getpid(), isFatalSignal(sigNum) ? 1 : 0);

    // Record fatal/crash signals so doForkExec() can re-raise after all
    // children are reaped, ensuring DobbyInit itself dies by the signal.
    // Graceful-stop signals (SIGTERM etc.) are intentionally not recorded
    // so that normal container shutdown still results in WIFEXITED=true.
    if (isFatalSignal(sigNum))
    {
        gReceivedFatalSignal = sigNum;
        LOG_NFO("###DBG Step2: gReceivedFatalSignal set to %d - will re-raise after wait loop",
                sigNum);
    }
    else
    {
        LOG_NFO("###DBG Step2: sig=%d is NOT fatal (graceful-stop) - gReceivedFatalSignal stays 0",
                sigNum);
    }

    // ###DBG Step3: broadcast to all container processes via kill(-1, sig)
    LOG_NFO("###DBG Step3: broadcasting sig=%d to all container processes via kill(-1, sig)",
            sigNum);

    // forward the signal to all processes in the container
    kill(-1, sigNum);
}

int main(int argc, char * argv[])
{
    // install a signal handler for SIGTERM and friends, dobby sends a SIGTERM
    // first to ask the container to die, then "after a reasonable timeout"
    // sends a SIGKILL.
    const int sigNums[] = { SIGHUP, SIGINT, SIGQUIT, SIGILL, SIGABRT, SIGFPE,
                            SIGSEGV, SIGALRM, SIGTERM, SIGUSR1, SIGUSR2 };
    for (int sigNum : sigNums)
    {
        if (signal(sigNum, signalHandler) != nullptr)
        {
            LOG_ERR("failed to install handler for signal %d (%d - %s)",
                    sigNum, errno, strerror(errno));
            // should this be fatal ?
        }
    }

    return doForkExec(argc, argv);
}
