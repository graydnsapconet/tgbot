#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
// enable http
#define TESTING

#include "test.h"
#include "../src/bot.h"
#include "../src/config.h"
#include "../lib/cJSON.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// mock server
typedef struct {
    pid_t pid;
    int port;
} MockServer;
static MockServer start_mock(const char *scenario)
{
    MockServer ms = {.pid = -1, .port = 0};

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return ms;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return ms;
    }

    if (pid == 0) {
        // child: redirect stdout to pipe write end
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        if (scenario) {
            execlp("python3", "python3", "mock_tg_server.py", "--scenario", scenario, NULL);
        } else {
            execlp("python3", "python3", "mock_tg_server.py", NULL);
        }
        _exit(127);
    }

    // parent: read port from pipe
    close(pipefd[1]);
    char buf[32] = {0};
    ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
    close(pipefd[0]);

    if (n > 0) {
        ms.port = atoi(buf);
    }
    ms.pid = pid;

    // give server time to fully start
    usleep(200000);
    return ms;
}
static void stop_mock(MockServer *ms)
{
    if (ms->pid > 0) {
        kill(ms->pid, SIGTERM);
        int status;
        waitpid(ms->pid, &status, 0);
        ms->pid = -1;
    }
}
static BotHandle *make_test_bot(int port)
{
    BotHandle *bot = bot_init("TESTTOKEN123");
    if (!bot) {
        return NULL;
    }
    char base[128];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/bot", port);
    bot_set_api_base(bot, base);
    bot_set_allow_http(bot, 1);
    return bot;
}

// tests

// "getMe" against mock server
TEST(bot_get_me_mock)
{
    MockServer ms = start_mock(NULL);
    ASSERT(ms.port > 0);

    BotHandle *bot = make_test_bot(ms.port);
    ASSERT_NOT_NULL(bot);

    cJSON *me = bot_get_me(bot);
    ASSERT_NOT_NULL(me);

    const cJSON *result = cJSON_GetObjectItemCaseSensitive(me, "result");
    ASSERT_NOT_NULL(result);
    const cJSON *username = cJSON_GetObjectItemCaseSensitive(result, "username");
    ASSERT(cJSON_IsString(username));
    ASSERT_STR_EQ(username->valuestring, "test_bot");

    cJSON_Delete(me);
    bot_cleanup(bot);
    stop_mock(&ms);
}

// "sendMessage" against mock server
TEST(bot_send_message_mock)
{
    MockServer ms = start_mock(NULL);
    ASSERT(ms.port > 0);

    BotHandle *bot = make_test_bot(ms.port);
    ASSERT_NOT_NULL(bot);

    int rc = bot_send_message(bot, 42, "hello from test");
    ASSERT_EQ(rc, 0);

    bot_cleanup(bot);
    stop_mock(&ms);
}

// "getUpdates" against mock server (empty result)
TEST(bot_get_updates_mock)
{
    MockServer ms = start_mock(NULL);
    ASSERT(ms.port > 0);

    BotHandle *bot = make_test_bot(ms.port);
    ASSERT_NOT_NULL(bot);

    cJSON *updates = bot_get_updates(bot, 0, 1, 10);
    ASSERT_NOT_NULL(updates);

    const cJSON *result = cJSON_GetObjectItemCaseSensitive(updates, "result");
    ASSERT(cJSON_IsArray(result));
    ASSERT_EQ(cJSON_GetArraySize(result), 0);

    cJSON_Delete(updates);
    bot_cleanup(bot);
    stop_mock(&ms);
}

// connection refused - no listener on port
TEST(bot_connection_refused)
{
    BotHandle *bot = bot_init("TESTTOKEN123");
    ASSERT_NOT_NULL(bot);

    // use a port that's very unlikely to have anything listening
    char base[128];
    snprintf(base, sizeof(base), "http://127.0.0.1:19999/bot");
    bot_set_api_base(bot, base);
    bot_set_allow_http(bot, 1);

    cJSON *me = bot_get_me(bot);
    ASSERT_NULL(me); // should fail gracefully

    bot_cleanup(bot);
}

// webhook secret mismatch - wrong/missing secret -> 403
TEST(bot_invalid_token_401)
{
    MockServer ms = start_mock("401-unauthorized");
    ASSERT(ms.port > 0);

    BotHandle *bot = make_test_bot(ms.port);
    ASSERT_NOT_NULL(bot);

    cJSON *updates = bot_get_updates(bot, 0, 1, 10);
    ASSERT_NULL(updates); // mock returns 401, bot should fail

    bot_cleanup(bot);
    stop_mock(&ms);
}

// partial read - mock sends truncated JSON
TEST(bot_partial_read_no_crash)
{
    MockServer ms = start_mock("partial-read");
    ASSERT(ms.port > 0);

    BotHandle *bot = make_test_bot(ms.port);
    ASSERT_NOT_NULL(bot);

    cJSON *updates = bot_get_updates(bot, 0, 1, 10);
    // should fail gracefully (partial JSON), not crash
    ASSERT_NULL(updates);

    bot_cleanup(bot);
    stop_mock(&ms);
}

int main(void)
{
    printf("=== test_bot ===\n");
    return test_summarise();
}
