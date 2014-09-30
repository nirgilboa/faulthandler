/*
 * faulthandler module
 *
 * Written by Victor Stinner.
 */

#include "Python.h"
#include "pythread.h"
#include <signal.h>
#ifdef MS_WINDOWS
#  include <windows.h>
#endif
#ifdef HAVE_SYS_RESOURCE_H
#  include <sys/resource.h>
#endif

#define VERSION 0x204

/* Allocate at maximum 100 MB of the stack to raise the stack overflow */
#define STACK_OVERFLOW_MAX_SIZE (100*1024*1024)

#ifdef SIGALRM
#  define FAULTHANDLER_LATER
#endif

#ifndef MS_WINDOWS
   /* sigaltstack() is not available on Windows */
#  define HAVE_SIGALTSTACK

   /* register() is useless on Windows, because only SIGSEGV, SIGABRT and
      SIGILL can be handled by the process, and these signals can only be used
      with enable(), not using register() */
#  define FAULTHANDLER_USER
#endif

#if PY_MAJOR_VERSION >= 3
#  define PYINT_CHECK PyLong_Check
#  define PYINT_ASLONG PyLong_AsLong
#else
#  define PYINT_CHECK PyInt_Check
#  define PYINT_ASLONG PyInt_AsLong
#endif

#define PUTS(fd, str) write(fd, str, strlen(str))


#ifdef HAVE_SIGACTION
typedef struct sigaction _Py_sighandler_t;
#else
typedef PyOS_sighandler_t _Py_sighandler_t;
#endif

typedef struct {
    int signum;
    int enabled;
    const char* name;
    _Py_sighandler_t previous;
    int all_threads;
} fault_handler_t;

static struct {
    int enabled;
    PyObject *file;
    int fd;
    int all_threads;
    PyInterpreterState *interp;
} fatal_error = {0, NULL, -1, 0};

#ifdef FAULTHANDLER_LATER
static struct {
    PyObject *file;
    int fd;
    int timeout;
    int repeat;
    PyInterpreterState *interp;
    int exit;
    char *header;
    size_t header_len;
} fault_alarm;
#endif

#ifdef FAULTHANDLER_USER
typedef struct {
    int enabled;
    PyObject *file;
    int fd;
    int all_threads;
    int chain;
    _Py_sighandler_t previous;
    PyInterpreterState *interp;
} user_signal_t;

static user_signal_t *user_signals;

/* the following macros come from Python: Modules/signalmodule.c of Python 3.3 */
#if defined(PYOS_OS2) && !defined(PYCC_GCC)
#define NSIG 12
#endif
#ifndef NSIG
# if defined(_NSIG)
#  define NSIG _NSIG            /* For BSD/SysV */
# elif defined(_SIGMAX)
#  define NSIG (_SIGMAX + 1)    /* For QNX */
# elif defined(SIGMAX)
#  define NSIG (SIGMAX + 1)     /* For djgpp */
# else
#  define NSIG 64               /* Use a reasonable default value */
# endif
#endif

static void faulthandler_user(int signum);
#endif /* FAULTHANDLER_USER */


static fault_handler_t faulthandler_handlers[] = {
#ifdef SIGBUS
    {SIGBUS, 0, "Bus error", },
#endif
#ifdef SIGILL
    {SIGILL, 0, "Illegal instruction", },
#endif
    {SIGFPE, 0, "Floating point exception", },
    {SIGABRT, 0, "Aborted", },
    /* define SIGSEGV at the end to make it the default choice if searching the
       handler fails in faulthandler_fatal_error() */
    {SIGSEGV, 0, "Segmentation fault", }
};
static const unsigned char faulthandler_nsignals = \
    sizeof(faulthandler_handlers) / sizeof(faulthandler_handlers[0]);

#ifdef HAVE_SIGALTSTACK
static stack_t stack;
#endif

/* Forward */
static void faulthandler_unload(void);

/* from traceback.c */
extern void _Py_DumpTraceback(int fd, PyThreadState *tstate);
extern const char* _Py_DumpTracebackThreads(
    int fd,
    PyInterpreterState *interp,
    PyThreadState *current_thread);

/* Get the file descriptor of a file by calling its fileno() method and then
   call its flush() method.

   If file is NULL or Py_None, use sys.stderr as the new file.

   On success, return the new file and write the file descriptor into *p_fd.
   On error, return NULL. */

static PyObject*
faulthandler_get_fileno(PyObject *file, int *p_fd)
{
    PyObject *result;
    long fd_long;
    int fd;

    if (file == NULL || file == Py_None) {
        file = PySys_GetObject("stderr");
        if (file == NULL) {
            PyErr_SetString(PyExc_RuntimeError, "unable to get sys.stderr");
            return NULL;
        }
        if (file == Py_None) {
            PyErr_SetString(PyExc_RuntimeError, "sys.stderr is None");
            return NULL;
        }
    }

    result = PyObject_CallMethod(file, "fileno", "");
    if (result == NULL)
        return NULL;

    fd = -1;
    if (PYINT_CHECK(result)) {
        fd_long = PYINT_ASLONG(result);
        if (0 < fd_long && fd_long < INT_MAX)
            fd = (int)fd_long;
    }
    Py_DECREF(result);

    if (fd == -1) {
        PyErr_SetString(PyExc_RuntimeError,
                        "file.fileno() is not a valid file descriptor");
        return NULL;
    }

    result = PyObject_CallMethod(file, "flush", "");
    if (result != NULL)
        Py_DECREF(result);
    else {
        /* ignore flush() error */
        PyErr_Clear();
    }
    *p_fd = fd;
    return file;
}

/* Get the state of the current thread: only call this function if the current
   thread holds the GIL. Raise an exception on error. */
static PyThreadState*
get_thread_state(void)
{
    PyThreadState *tstate = PyThreadState_Get();
    if (tstate == NULL) {
        PyErr_SetString(PyExc_RuntimeError,
                        "unable to get the current thread state");
        return NULL;
    }
    return tstate;
}

static PyObject*
faulthandler_dump_traceback_py(PyObject *self,
                               PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"file", "all_threads", NULL};
    PyObject *file = NULL;
    int all_threads = 1;
    PyThreadState *tstate;
    const char *errmsg;
    int fd;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
        "|Oi:dump_traceback", kwlist,
        &file, &all_threads))
        return NULL;

    file = faulthandler_get_fileno(file, &fd);
    if (file == NULL)
        return NULL;

    tstate = get_thread_state();
    if (tstate == NULL)
        return NULL;

    if (all_threads) {
        errmsg = _Py_DumpTracebackThreads(fd, tstate->interp, tstate);
        if (errmsg != NULL) {
            PyErr_SetString(PyExc_RuntimeError, errmsg);
            return NULL;
        }
    }
    else {
        _Py_DumpTraceback(fd, tstate);
    }
    Py_RETURN_NONE;
}


/* Handler for SIGSEGV, SIGFPE, SIGABRT, SIGBUS and SIGILL signals.

   Display the current Python traceback, restore the previous handler and call
   the previous handler.

   On Windows, don't explicitly call the previous handler, because the Windows
   signal handler would not be called (for an unknown reason). The execution of
   the program continues at faulthandler_fatal_error() exit, but the same
   instruction will raise the same fault (signal), and so the previous handler
   will be called.

   This function is signal-safe and should only call signal-safe functions. */

static void
faulthandler_fatal_error(int signum)
{
    const int fd = fatal_error.fd;
    unsigned int i;
    fault_handler_t *handler = NULL;
    PyThreadState *tstate;
    int save_errno = errno;

    if (!fatal_error.enabled)
        return;

    for (i=0; i < faulthandler_nsignals; i++) {
        handler = &faulthandler_handlers[i];
        if (handler->signum == signum)
            break;
    }
    if (handler == NULL) {
        /* faulthandler_nsignals == 0 (unlikely) */
        return;
    }

    /* restore the previous handler */
#ifdef HAVE_SIGACTION
    (void)sigaction(signum, &handler->previous, NULL);
#else
    (void)signal(signum, handler->previous);
#endif
    handler->enabled = 0;

    PUTS(fd, "Fatal Python error: ");
    PUTS(fd, handler->name);
    PUTS(fd, "\n\n");

#ifdef WITH_THREAD
    /* SIGSEGV, SIGFPE, SIGABRT, SIGBUS and SIGILL are synchronous signals and
       are thus delivered to the thread that caused the fault. Get the Python
       thread state of the current thread.

       PyThreadState_Get() doesn't give the state of the thread that caused the
       fault if the thread released the GIL, and so this function cannot be
       used. Read the thread local storage (TLS) instead: call
       PyGILState_GetThisThreadState(). */
    tstate = PyGILState_GetThisThreadState();
#else
    tstate = PyThreadState_Get();
#endif

    if (fatal_error.all_threads)
        _Py_DumpTracebackThreads(fd, fatal_error.interp, tstate);
    else {
        if (tstate != NULL)
            _Py_DumpTraceback(fd, tstate);
    }

    errno = save_errno;
#ifdef MS_WINDOWS
    if (signum == SIGSEGV) {
        /* don't explicitly call the previous handler for SIGSEGV in this signal
           handler, because the Windows signal handler would not be called */
        return;
    }
#endif
    /* call the previous signal handler: it is called immediately if we use
       sigaction() thanks to SA_NODEFER flag, otherwise it is deferred */
    raise(signum);
}

/* Install the handler for fatal signals, faulthandler_fatal_error(). */

static PyObject*
faulthandler_enable(PyObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"file", "all_threads", NULL};
    PyObject *file = NULL;
    int all_threads = 1;
    unsigned int i;
    fault_handler_t *handler;
#ifdef HAVE_SIGACTION
    struct sigaction action;
#endif
    int err;
    int fd;
    PyThreadState *tstate;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
        "|Oi:enable", kwlist, &file, &all_threads))
        return NULL;

    file = faulthandler_get_fileno(file, &fd);
    if (file == NULL)
        return NULL;

    tstate = get_thread_state();
    if (tstate == NULL)
        return NULL;

    Py_XDECREF(fatal_error.file);
    Py_INCREF(file);
    fatal_error.file = file;
    fatal_error.fd = fd;
    fatal_error.all_threads = all_threads;
    fatal_error.interp = tstate->interp;

    if (!fatal_error.enabled) {
        fatal_error.enabled = 1;

        for (i=0; i < faulthandler_nsignals; i++) {
            handler = &faulthandler_handlers[i];
#ifdef HAVE_SIGACTION
            action.sa_handler = faulthandler_fatal_error;
            sigemptyset(&action.sa_mask);
            /* Do not prevent the signal from being received from within
               its own signal handler */
            action.sa_flags = SA_NODEFER;
#ifdef HAVE_SIGALTSTACK
            if (stack.ss_sp != NULL) {
                /* Call the signal handler on an alternate signal stack
                   provided by sigaltstack() */
                action.sa_flags |= SA_ONSTACK;
            }
#endif
            err = sigaction(handler->signum, &action, &handler->previous);
#else
            handler->previous = signal(handler->signum,
                                       faulthandler_fatal_error);
            err = (handler->previous == SIG_ERR);
#endif
            if (err) {
                PyErr_SetFromErrno(PyExc_RuntimeError);
                return NULL;
            }
            handler->enabled = 1;
        }
    }
    Py_RETURN_NONE;
}

static void
faulthandler_disable(void)
{
    unsigned int i;
    fault_handler_t *handler;

    if (fatal_error.enabled) {
        fatal_error.enabled = 0;
        for (i=0; i < faulthandler_nsignals; i++) {
            handler = &faulthandler_handlers[i];
            if (!handler->enabled)
                continue;
#ifdef HAVE_SIGACTION
            (void)sigaction(handler->signum, &handler->previous, NULL);
#else
            (void)signal(handler->signum, handler->previous);
#endif
            handler->enabled = 0;
        }
    }

    Py_CLEAR(fatal_error.file);
}

static PyObject*
faulthandler_disable_py(PyObject *self)
{
    if (!fatal_error.enabled) {
        Py_INCREF(Py_False);
        return Py_False;
    }
    faulthandler_disable();
    Py_INCREF(Py_True);
    return Py_True;
}

static PyObject*
faulthandler_is_enabled(PyObject *self)
{
    return PyBool_FromLong(fatal_error.enabled);
}

#ifdef FAULTHANDLER_LATER
/* Handler of the SIGALRM signal.

   Dump the traceback of the current thread, or of all threads if
   fault_alarm.all_threads is true. On success, register itself again if
   fault_alarm.repeat is true.

   This function is signal safe and should only call signal safe functions. */

static void
faulthandler_alarm(int signum)
{
    PyThreadState *tstate;
    const char* errmsg;
    int ok;

    write(fault_alarm.fd, fault_alarm.header, fault_alarm.header_len);

    /* PyThreadState_Get() doesn't give the state of the current thread if
       the thread doesn't hold the GIL. Read the thread local storage (TLS)
       instead: call PyGILState_GetThisThreadState(). */
    tstate = PyGILState_GetThisThreadState();

    errmsg = _Py_DumpTracebackThreads(fault_alarm.fd, fault_alarm.interp, tstate);
    ok = (errmsg == NULL);

    if (ok && fault_alarm.repeat)
        alarm(fault_alarm.timeout);
    else
        /* don't call Py_CLEAR() here because it may call _Py_Dealloc() which
           is not signal safe */
        alarm(0);

    if (fault_alarm.exit)
        _exit(1);
}

static char*
format_timeout(double timeout)
{
    unsigned long us, sec, min, hour;
    double intpart, fracpart;
    char buffer[100];

    fracpart = modf(timeout, &intpart);
    sec = (unsigned long)intpart;
    us = (unsigned long)(fracpart * 1e6);
    min = sec / 60;
    sec %= 60;
    hour = min / 60;
    min %= 60;

    if (us != 0)
        PyOS_snprintf(buffer, sizeof(buffer),
                      "Timeout (%lu:%02lu:%02lu.%06lu)!\n",
                      hour, min, sec, us);
    else
        PyOS_snprintf(buffer, sizeof(buffer),
                      "Timeout (%lu:%02lu:%02lu)!\n",
                      hour, min, sec);

    return strdup(buffer);
}

static PyObject*
faulthandler_dump_traceback_later(PyObject *self,
                                  PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"timeout", "repeat", "file", "exit", NULL};
    int timeout;
    PyOS_sighandler_t previous;
    int repeat = 0;
    PyObject *file = NULL;
    int exit = 0;
    PyThreadState *tstate;
    int fd;
    char *header;
    size_t header_len;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
        "i|iOi:dump_traceback_later", kwlist,
        &timeout, &repeat, &file, &exit))
        return NULL;
    if (timeout <= 0) {
        PyErr_SetString(PyExc_ValueError, "timeout must be greater than 0");
        return NULL;
    }

    tstate = get_thread_state();
    if (tstate == NULL)
        return NULL;

    file = faulthandler_get_fileno(file, &fd);
    if (file == NULL)
        return NULL;

    /* format the timeout */
    header = format_timeout(timeout);
    if (header == NULL)
        return PyErr_NoMemory();
    header_len = strlen(header);

    previous = signal(SIGALRM, faulthandler_alarm);
    if (previous == SIG_ERR) {
        PyErr_SetString(PyExc_RuntimeError, "unable to set SIGALRM handler");
        free(header);
        return NULL;
    }

    Py_XDECREF(fault_alarm.file);
    Py_INCREF(file);
    fault_alarm.file = file;
    fault_alarm.fd = fd;
    fault_alarm.timeout = timeout;
    fault_alarm.repeat = repeat;
    fault_alarm.interp = tstate->interp;
    fault_alarm.exit = exit;
    fault_alarm.header = header;
    fault_alarm.header_len = header_len;

    alarm(timeout);

    Py_RETURN_NONE;
}

static PyObject*
faulthandler_cancel_dump_traceback_later_py(PyObject *self)
{
    alarm(0);
    Py_CLEAR(fault_alarm.file);
    free(fault_alarm.header);
    fault_alarm.header = NULL;
    Py_RETURN_NONE;
}
#endif /* FAULTHANDLER_LATER */

#ifdef FAULTHANDLER_USER
static int
faulthandler_register(int signum, int chain, _Py_sighandler_t *p_previous)
{
#ifdef HAVE_SIGACTION
    struct sigaction action;
    action.sa_handler = faulthandler_user;
    sigemptyset(&action.sa_mask);
    /* if the signal is received while the kernel is executing a system
       call, try to restart the system call instead of interrupting it and
       return EINTR. */
    action.sa_flags = SA_RESTART;
    if (chain) {
        /* do not prevent the signal from being received from within its
           own signal handler */
        action.sa_flags = SA_NODEFER;
    }
#ifdef HAVE_SIGALTSTACK
    if (stack.ss_sp != NULL) {
        /* Call the signal handler on an alternate signal stack
           provided by sigaltstack() */
        action.sa_flags |= SA_ONSTACK;
    }
#endif
    return sigaction(signum, &action, p_previous);
#else
    _Py_sighandler_t previous;
    previous = signal(signum, faulthandler_user);
    if (p_previous != NULL)
        *p_previous = previous;
    return (previous == SIG_ERR);
#endif
}

/* Handler of user signals (e.g. SIGUSR1).

   Dump the traceback of the current thread, or of all threads if
   thread.all_threads is true.

   This function is signal safe and should only call signal safe functions. */

static void
faulthandler_user(int signum)
{
    user_signal_t *user;
    PyThreadState *tstate;
    int save_errno = errno;

    user = &user_signals[signum];
    if (!user->enabled)
        return;

#ifdef WITH_THREAD
    /* PyThreadState_Get() doesn't give the state of the current thread if
       the thread doesn't hold the GIL. Read the thread local storage (TLS)
       instead: call PyGILState_GetThisThreadState(). */
    tstate = PyGILState_GetThisThreadState();
#else
    tstate = PyThreadState_Get();
#endif

    if (user->all_threads)
        _Py_DumpTracebackThreads(user->fd, user->interp, tstate);
    else {
        if (tstate != NULL)
            _Py_DumpTraceback(user->fd, tstate);
    }
#ifdef HAVE_SIGACTION
    if (user->chain) {
        (void)sigaction(signum, &user->previous, NULL);
        errno = save_errno;

        /* call the previous signal handler */
        raise(signum);

        save_errno = errno;
        (void)faulthandler_register(signum, user->chain, NULL);
        errno = save_errno;
    }
#else
    if (user->chain) {
        errno = save_errno;
        /* call the previous signal handler */
        user->previous(signum);
    }
#endif
}

static int
check_signum(int signum)
{
    unsigned int i;

    for (i=0; i < faulthandler_nsignals; i++) {
        if (faulthandler_handlers[i].signum == signum) {
            PyErr_Format(PyExc_RuntimeError,
                         "signal %i cannot be registered, "
                         "use enable() instead",
                         signum);
            return 0;
        }
    }
    if (signum < 1 || NSIG <= signum) {
        PyErr_SetString(PyExc_ValueError, "signal number out of range");
        return 0;
    }
    return 1;
}

static PyObject*
faulthandler_register_py(PyObject *self,
                         PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"signum", "file", "all_threads", "chain", NULL};
    int signum;
    PyObject *file = NULL;
    int all_threads = 1;
    int chain = 0;
    int fd;
    user_signal_t *user;
    _Py_sighandler_t previous;
    PyThreadState *tstate;
    int err;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
        "i|Oii:register", kwlist,
        &signum, &file, &all_threads, &chain))
        return NULL;

    if (!check_signum(signum))
        return NULL;

    tstate = get_thread_state();
    if (tstate == NULL)
        return NULL;

    file = faulthandler_get_fileno(file, &fd);
    if (file == NULL)
        return NULL;

    if (user_signals == NULL) {
        user_signals = calloc(NSIG, sizeof(user_signal_t));
        if (user_signals == NULL)
            return PyErr_NoMemory();
    }
    user = &user_signals[signum];

    if (!user->enabled) {
        err = faulthandler_register(signum, chain, &previous);
        if (err) {
            PyErr_SetFromErrno(PyExc_OSError);
            return NULL;
        }

        user->previous = previous;
    }

    Py_XDECREF(user->file);
    Py_INCREF(file);
    user->file = file;
    user->fd = fd;
    user->all_threads = all_threads;
    user->chain = chain;
    user->interp = tstate->interp;
    user->enabled = 1;

    Py_RETURN_NONE;
}

static int
faulthandler_unregister(user_signal_t *user, int signum)
{
    if (!user->enabled)
        return 0;
    user->enabled = 0;
#ifdef HAVE_SIGACTION
    (void)sigaction(signum, &user->previous, NULL);
#else
    (void)signal(signum, user->previous);
#endif
    user->fd = -1;
    return 1;
}

static PyObject*
faulthandler_unregister_py(PyObject *self, PyObject *args)
{
    int signum;
    user_signal_t *user;
    int change;

    if (!PyArg_ParseTuple(args, "i:unregister", &signum))
        return NULL;

    if (!check_signum(signum))
        return NULL;

    if (user_signals == NULL)
        Py_RETURN_FALSE;

    user = &user_signals[signum];
    change = faulthandler_unregister(user, signum);
    Py_CLEAR(user->file);
    return PyBool_FromLong(change);
}
#endif   /* FAULTHANDLER_USER */


static void
faulthandler_suppress_crash_report(void)
{
#ifdef MS_WINDOWS
    UINT mode;

    /* Configure Windows to not display the Windows Error Reporting dialog */
    mode = SetErrorMode(SEM_NOGPFAULTERRORBOX);
    SetErrorMode(mode | SEM_NOGPFAULTERRORBOX);
#endif

#ifdef HAVE_SYS_RESOURCE_H
    struct rlimit rl;

    /* Disable creation of core dump */
    if (getrlimit(RLIMIT_CORE, &rl) != 0) {
        rl.rlim_cur = 0;
        setrlimit(RLIMIT_CORE, &rl);
    }
#endif

#ifdef _MSC_VER
    /* Visual Studio: configure abort() to not display an error message nor
       open a popup asking to report the fault. */
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
}

static PyObject *
faulthandler_read_null(PyObject *self, PyObject *args)
{
    volatile int *x;
    volatile int y;

    faulthandler_suppress_crash_report();
    x = NULL;
    y = *x;
    return PyLong_FromLong(y);

}

static void
faulthandler_raise_sigsegv(void)
{
    faulthandler_suppress_crash_report();
#if defined(MS_WINDOWS)
    /* For SIGSEGV, faulthandler_fatal_error() restores the previous signal
       handler and then gives back the execution flow to the program (without
       explicitly calling the previous error handler). In a normal case, the
       SIGSEGV was raised by the kernel because of a fault, and so if the
       program retries to execute the same instruction, the fault will be
       raised again.

       Here the fault is simulated by a fake SIGSEGV signal raised by the
       application. We have to raise SIGSEGV at lease twice: once for
       faulthandler_fatal_error(), and one more time for the previous signal
       handler. */
    while(1)
        raise(SIGSEGV);
#else
    raise(SIGSEGV);
#endif
}

static PyObject *
faulthandler_sigsegv(PyObject *self, PyObject *args)
{
    int release_gil = 0;
    if (!PyArg_ParseTuple(args, "|i:_read_null", &release_gil))
        return NULL;

    if (release_gil) {
        Py_BEGIN_ALLOW_THREADS
        faulthandler_raise_sigsegv();
        Py_END_ALLOW_THREADS
    } else {
        faulthandler_raise_sigsegv();
    }
    Py_RETURN_NONE;
}

static PyObject *
faulthandler_sigfpe(PyObject *self, PyObject *args)
{
    /* Do an integer division by zero: raise a SIGFPE on Intel CPU, but not on
       PowerPC. Use volatile to disable compile-time optimizations. */
    volatile int x = 1, y = 0, z;
    faulthandler_suppress_crash_report();
    z = x / y;
    /* If the division by zero didn't raise a SIGFPE (e.g. on PowerPC),
       raise it manually. */
    raise(SIGFPE);
    /* This line is never reached, but we pretend to make something with z
       to silence a compiler warning. */
    return PyLong_FromLong(z);
}

static PyObject *
faulthandler_sigabrt(PyObject *self, PyObject *args)
{
    faulthandler_suppress_crash_report();
    abort();
    Py_RETURN_NONE;
}

static PyObject *
faulthandler_raise_signal(PyObject *self, PyObject *args)
{
    int signum, err;

    if (PyArg_ParseTuple(args, "i:raise_signal", &signum) < 0)
        return NULL;

    faulthandler_suppress_crash_report();

    err = raise(signum);
    if (err)
        return PyErr_SetFromErrno(PyExc_OSError);

    if (PyErr_CheckSignals() < 0)
        return NULL;

    Py_RETURN_NONE;
}

static PyObject *
faulthandler_fatal_error_py(PyObject *self, PyObject *args)
{
    char *message;
#if PY_MAJOR_VERSION >= 3
    if (!PyArg_ParseTuple(args, "y:fatal_error", &message))
        return NULL;
#else
    if (!PyArg_ParseTuple(args, "s:fatal_error", &message))
        return NULL;
#endif
    Py_FatalError(message);
    Py_RETURN_NONE;
}

#if defined(HAVE_SIGALTSTACK) && defined(HAVE_SIGACTION)
static void*
stack_overflow(void *min_sp, void *max_sp, size_t *depth)
{
    /* allocate 4096 bytes on the stack at each call */
    unsigned char buffer[4096];
    void *sp = &buffer;
    *depth += 1;
    if (sp < min_sp || max_sp < sp)
        return sp;
    buffer[0] = 1;
    buffer[4095] = 0;
    return stack_overflow(min_sp, max_sp, depth);
}

static PyObject *
faulthandler_stack_overflow(PyObject *self)
{
    size_t depth, size;
    char *sp = (char *)&depth, *stop;

    faulthandler_suppress_crash_report();
    depth = 0;
    stop = stack_overflow(sp - STACK_OVERFLOW_MAX_SIZE,
                          sp + STACK_OVERFLOW_MAX_SIZE,
                          &depth);
    if (sp < stop)
        size = stop - sp;
    else
        size = sp - stop;
    PyErr_Format(PyExc_RuntimeError,
        "unable to raise a stack overflow (allocated %zu bytes "
        "on the stack, %zu recursive calls)",
        size, depth);
    return NULL;
}
#endif

#if PY_MAJOR_VERSION >= 3
static int
faulthandler_traverse(PyObject *module, visitproc visit, void *arg)
{
#ifdef FAULTHANDLER_USER
    unsigned int signum;
#endif

#ifdef FAULTHANDLER_LATER
    Py_VISIT(fault_alarm.file);
#endif
#ifdef FAULTHANDLER_USER
    if (user_signals != NULL) {
        for (signum=0; signum < NSIG; signum++)
            Py_VISIT(user_signals[signum].file);
    }
#endif
    Py_VISIT(fatal_error.file);
    return 0;
}
#endif

PyDoc_STRVAR(module_doc,
"faulthandler module.");

static PyMethodDef module_methods[] = {
    {"enable",
     (PyCFunction)faulthandler_enable, METH_VARARGS|METH_KEYWORDS,
     PyDoc_STR("enable(file=sys.stderr, all_threads=True): "
               "enable the fault handler")},
    {"disable", (PyCFunction)faulthandler_disable_py, METH_NOARGS,
     PyDoc_STR("disable(): disable the fault handler")},
    {"is_enabled", (PyCFunction)faulthandler_is_enabled, METH_NOARGS,
     PyDoc_STR("is_enabled()->bool: check if the handler is enabled")},
    {"dump_traceback",
     (PyCFunction)faulthandler_dump_traceback_py, METH_VARARGS|METH_KEYWORDS,
     PyDoc_STR("dump_traceback(file=sys.stderr, all_threads=True): "
               "dump the traceback of the current thread, or of all threads "
               "if all_threads is True, into file")},
#ifdef FAULTHANDLER_LATER
    {"dump_traceback_later",
     (PyCFunction)faulthandler_dump_traceback_later, METH_VARARGS|METH_KEYWORDS,
     PyDoc_STR("dump_traceback_later(timeout, repeat=False, file=sys.stderrn, exit=False):\n"
               "dump the traceback of all threads in timeout seconds,\n"
               "or each timeout seconds if repeat is True. If exit is True, "
               "call _exit(1) which is not safe.")},
    {"cancel_dump_traceback_later",
     (PyCFunction)faulthandler_cancel_dump_traceback_later_py, METH_NOARGS,
     PyDoc_STR("cancel_dump_traceback_later():\ncancel the previous call "
               "to dump_traceback_later().")},
#endif

#ifdef FAULTHANDLER_USER
    {"register",
     (PyCFunction)faulthandler_register_py, METH_VARARGS|METH_KEYWORDS,
     PyDoc_STR("register(signum, file=sys.stderr, all_threads=True, chain=False): "
               "register an handler for the signal 'signum': dump the "
               "traceback of the current thread, or of all threads if "
               "all_threads is True, into file")},
    {"unregister",
     faulthandler_unregister_py, METH_VARARGS|METH_KEYWORDS,
     PyDoc_STR("unregister(signum): unregister the handler of the signal "
                "'signum' registered by register()")},
#endif

    {"_read_null", faulthandler_read_null, METH_NOARGS,
     PyDoc_STR("_read_null(): read from NULL, raise "
               "a SIGSEGV or SIGBUS signal depending on the platform")},
    {"_sigsegv", faulthandler_sigsegv, METH_VARARGS,
     PyDoc_STR("_sigsegv(release_gil=False): raise a SIGSEGV signal")},
    {"_sigabrt", faulthandler_sigabrt, METH_NOARGS,
     PyDoc_STR("_sigabrt(): raise a SIGABRT signal")},
    {"_sigfpe", (PyCFunction)faulthandler_sigfpe, METH_NOARGS,
     PyDoc_STR("_sigfpe(): raise a SIGFPE signal")},
    {"_raise_signal", (PyCFunction)faulthandler_raise_signal, METH_VARARGS,
     PyDoc_STR("raise_signal(signum): raise a signal")},
    {"_fatal_error", faulthandler_fatal_error_py, METH_VARARGS,
     PyDoc_STR("_fatal_error(message): call Py_FatalError(message)")},
#if defined(HAVE_SIGALTSTACK) && defined(HAVE_SIGACTION)
    {"_stack_overflow", (PyCFunction)faulthandler_stack_overflow, METH_NOARGS,
     PyDoc_STR("_stack_overflow(): recursive call to raise a stack overflow")},
#endif
    {NULL, NULL}  /* sentinel */
};

#if PY_MAJOR_VERSION >= 3
static struct PyModuleDef module_def = {
    PyModuleDef_HEAD_INIT,
    "faulthandler",
    module_doc,
    0, /* non negative size to be able to unload the module */
    module_methods,
    NULL,
    faulthandler_traverse,
    NULL,
    NULL
};
#endif


PyMODINIT_FUNC
#if PY_MAJOR_VERSION >= 3
PyInit_faulthandler(void)
#else
initfaulthandler(void)
#endif
{
    PyObject *m, *version;
#ifdef HAVE_SIGALTSTACK
    int err;
#endif

#if PY_MAJOR_VERSION >= 3
    m = PyModule_Create(&module_def);
#else
    m = Py_InitModule3("faulthandler", module_methods, module_doc);
#endif
    if (m == NULL) {
#if PY_MAJOR_VERSION >= 3
        return NULL;
#else
        return;
#endif
    }

#ifdef HAVE_SIGALTSTACK
    /* Try to allocate an alternate stack for faulthandler() signal handler to
     * be able to allocate memory on the stack, even on a stack overflow. If it
     * fails, ignore the error. */
    stack.ss_flags = 0;
    stack.ss_size = SIGSTKSZ;
    stack.ss_sp = PyMem_Malloc(stack.ss_size);
    if (stack.ss_sp != NULL) {
        err = sigaltstack(&stack, NULL);
        if (err) {
            PyMem_Free(stack.ss_sp);
            stack.ss_sp = NULL;
        }
    }
#endif

    (void)Py_AtExit(faulthandler_unload);

    version = Py_BuildValue("(ii)", VERSION >> 8, VERSION & 0xFF);
    if (version == NULL)
        goto error;
    PyModule_AddObject(m, "version", version);

#if PY_MAJOR_VERSION >= 3
    version = PyUnicode_FromFormat("%i.%i", VERSION >> 8, VERSION & 0xFF);
#else
    version = PyString_FromFormat("%i.%i", VERSION >> 8, VERSION & 0xFF);
#endif
    if (version == NULL)
        goto error;
    PyModule_AddObject(m, "__version__", version);

#if PY_MAJOR_VERSION >= 3
    return m;
#else
    return;
#endif

error:
#if PY_MAJOR_VERSION >= 3
    Py_DECREF(m);
    return NULL;
#else
    return;
#endif
}

static void
faulthandler_unload(void)
{
#ifdef FAULTHANDLER_USER
    unsigned int signum;
#endif

#ifdef FAULTHANDLER_LATER
    /* later */
    alarm(0);
    if (fault_alarm.header != NULL) {
        free(fault_alarm.header);
        fault_alarm.header = NULL;
    }
    /* Don't call Py_CLEAR(fault_alarm.file): this function is called too late,
       by Py_AtExit(). Destroy a Python object here raise strange errors. */
#endif
#ifdef FAULTHANDLER_USER
    /* user */
    if (user_signals != NULL) {
        for (signum=0; signum < NSIG; signum++) {
            faulthandler_unregister(&user_signals[signum], signum);
            /* Don't call Py_CLEAR(user->file): this function is called too late,
               by Py_AtExit(). Destroy a Python object here raise strange
               errors. */
        }
        free(user_signals);
        user_signals = NULL;
    }
#endif

    /* don't release file: faulthandler_unload_fatal_error()
       is called too late */
    fatal_error.file = NULL;
    faulthandler_disable();
#ifdef HAVE_SIGALTSTACK
    if (stack.ss_sp != NULL) {
        PyMem_Free(stack.ss_sp);
        stack.ss_sp = NULL;
    }
#endif
}
