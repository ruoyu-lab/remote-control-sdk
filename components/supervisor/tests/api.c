/* The supervisor's C functions (rcsdk/supervisor.h) compile as C and link:
 * their arguments are checked on every system; where there is no supervisor,
 * UNSUPPORTED. */
#include <rcsdk/supervisor.h>

#include <stdio.h>
#include <string.h>

/* The reason the most recent failing call gave, kept across later calls. */
static char lastReason[256];

static void keepReason(void) {
    if (rcs_get_last_call_error(lastReason, sizeof(lastReason)) != RCS_OK) {
        lastReason[0] = '\0';
    }
}

static int reason(const char* text) {
    return strstr(lastReason, text) != NULL;
}

/* Every failed check ends the test here, naming its line. */
static int failedAt(int line) {
    char now[256];
    rcs_get_last_call_error(now, sizeof(now));
    fprintf(stderr, "api.c:%d: check failed (last reason: \"%s\")\n", line, now[0] ? now : lastReason);
    return 1;
}

int main(void) {
    rcs_supervisor_config config = RCS_SUPERVISOR_CONFIG_INIT;
    rcs_supervisor_status status = RCS_SUPERVISOR_STATUS_INIT;
    rcs_supervisor supervisor = NULL;
    if (rcs_supervisor_create(&config, NULL, &supervisor) != RCS_ERROR_INVALID_ARGUMENT || supervisor) {
        return failedAt(__LINE__); /* No command line. */
    }
    /* The program is a full path with its extension, quoted when it holds a space: CreateProcess
     * would try "C:\Program" first for an unquoted one. Checked on every system. */
    {
        static const char* const refused[] = {
            "C:\\Program Files\\MyApp\\helper.exe --helper", /* Unquoted, with a space. */
            "helper.exe --helper",                           /* No full path. */
            "\"C:\\Program Files\\MyApp\\helper.exe --helper", /* The quote is not closed. */
            "\"C:\\Program Files\\MyApp\\helper\" --helper",   /* No extension. */
            "\"C:\\Program Files\\MyApp\\helper.exe\"--helper", /* Nothing between the program and its arguments. */
            /* A network share's or a device's path: whoever can write there would run as LocalSystem. */
            "\\\\server\\share\\helper.exe --helper",
            "\"\\\\?\\UNC\\server\\share\\helper.exe\" --helper",
            "\\\\.\\pipe\\helper.exe",
            "//server/share/helper.exe"
        };
        size_t i;
        for (i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i) {
            strcpy(config.command_line, refused[i]);
            if (rcs_supervisor_create(&config, NULL, &supervisor) != RCS_ERROR_INVALID_ARGUMENT || supervisor) {
                return failedAt(__LINE__);
            }
            keepReason();
            if (!reason("command_line")) {
                return failedAt(__LINE__);
            }
        }
    }
    strcpy(config.command_line, "\"C:\\Program Files\\MyApp\\helper.exe\" --helper");
    memset(config.helper_key, 9, sizeof(config.helper_key));
    config.account = 2;
    if (rcs_supervisor_create(&config, NULL, &supervisor) != RCS_ERROR_INVALID_ARGUMENT) {
        return failedAt(__LINE__);
    }
    config.account = RCS_HELPER_USER;
    config.restart_delay_ms = 10;
    if (rcs_supervisor_create(&config, NULL, &supervisor) != RCS_ERROR_INVALID_ARGUMENT) {
        return failedAt(__LINE__);
    }
    config.restart_delay_ms = 5000;
    config.restart_delay_max_ms = 1000; /* Below the delay it doubles from. */
    if (rcs_supervisor_create(&config, NULL, &supervisor) != RCS_ERROR_INVALID_ARGUMENT) {
        return failedAt(__LINE__);
    }
    config.restart_delay_max_ms = 30000;
    config.stop_grace_ms = 99999;
    if (rcs_supervisor_create(&config, NULL, &supervisor) != RCS_ERROR_INVALID_ARGUMENT) {
        return failedAt(__LINE__);
    }
    config.stop_grace_ms = 3000;
    config.os_session = 0xFFFFFFFFu; /* Names no session (0 is the console's). */
    if (rcs_supervisor_create(&config, NULL, &supervisor) != RCS_ERROR_INVALID_ARGUMENT || supervisor) {
        return failedAt(__LINE__);
    }
    keepReason();
    if (!reason("os_session")) {
        return failedAt(__LINE__);
    }
    config.os_session = 0;
    config.version = RCSDK_VERSION + 1;
    if (rcs_supervisor_create(&config, NULL, &supervisor) != RCS_ERROR_INVALID_ARGUMENT) {
        return failedAt(__LINE__);
    }
    config.version = RCSDK_VERSION;
#ifdef _WIN32
    /* Either account takes a LocalSystem process: moving a token to the console's session, and
     * taking the signed-in user's token, are LocalSystem's rights. A test runs as itself. */
    {
        const rcs_helper_account accounts[] = {RCS_HELPER_USER, RCS_HELPER_SYSTEM};
        size_t i;
        for (i = 0; i < 2; ++i) {
            rcs_result result;
            config.account = accounts[i];
            result = rcs_supervisor_create(&config, NULL, &supervisor);
            if (result == RCS_OK) {
                /* What may change does, also before start; what is fixed for its life does not. */
                rcs_supervisor_config changed = config;
                changed.os_session = 3;
                changed.restart_delay_ms = 500;
                if (rcs_supervisor_get_status(supervisor, &status) != RCS_OK || status.helper_running ||
                    rcs_supervisor_update(supervisor, &changed) != RCS_OK) {
                    return failedAt(__LINE__);
                }
                changed.account = accounts[1 - i];
                if (rcs_supervisor_update(supervisor, &changed) != RCS_ERROR_INVALID_ARGUMENT) {
                    return failedAt(__LINE__);
                }
                changed = config;
                changed.helper_key[31] ^= 1;
                if (rcs_supervisor_update(supervisor, &changed) != RCS_ERROR_INVALID_ARGUMENT ||
                    rcs_supervisor_destroy(supervisor) != RCS_OK) {
                    return failedAt(__LINE__);
                }
            } else if (result != RCS_ERROR_PERMISSION_REQUIRED || supervisor) {
                return failedAt(__LINE__);
            }
        }
    }
#else
    if (rcs_supervisor_create(&config, NULL, &supervisor) != RCS_ERROR_UNSUPPORTED || supervisor) {
        return failedAt(__LINE__);
    }
#endif
    if (rcs_supervisor_start(NULL) != RCS_ERROR_INVALID_ARGUMENT || rcs_supervisor_stop(NULL) != RCS_ERROR_INVALID_ARGUMENT ||
        rcs_supervisor_get_status(NULL, &status) != RCS_ERROR_INVALID_ARGUMENT ||
        rcs_supervisor_update(NULL, &config) != RCS_ERROR_INVALID_ARGUMENT ||
        rcs_supervisor_destroy(NULL) != RCS_OK) {
        return failedAt(__LINE__);
    }
    /* The sessions a supervisor can choose: a list like the others; Windows' only. */
    {
        rcs_os_session_info sessions[8];
        uint32_t sessionCount = 0;
        if (rcs_list_os_sessions(NULL, sizeof(rcs_os_session_info) - 4, 0, &sessionCount) !=
                RCS_ERROR_INVALID_ARGUMENT ||
            rcs_list_os_sessions(sessions, sizeof(rcs_os_session_info), 8, NULL) != RCS_ERROR_INVALID_ARGUMENT) {
            return failedAt(__LINE__);
        }
#ifdef _WIN32
        if (rcs_list_os_sessions(NULL, sizeof(rcs_os_session_info), 0, &sessionCount) != RCS_OK || !sessionCount) {
            return failedAt(__LINE__);
        }
#else
        if (rcs_list_os_sessions(sessions, sizeof(rcs_os_session_info), 8, &sessionCount) !=
            RCS_ERROR_UNSUPPORTED) {
            return failedAt(__LINE__);
        }
#endif
    }
    printf("The supervisor's C interface compiles as C and links\n");
    return 0;
}
