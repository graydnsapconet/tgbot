#pragma once

#include <signal.h>

// opaque handle for LLM chat completions via local LM Studio server
typedef struct LlmHandle LlmHandle;

// initialise an LLM handle pointing at a local OpenAI-compatible server.
// endpoint example: "http://127.0.0.1:1234"
// model: model id string (e.g. "qwen/qwen3-8b"), or NULL for server default
// returns NULL on failure.
LlmHandle *llm_init(const char *endpoint, const char *model);

// release all resources associated with the handle
void llm_cleanup(LlmHandle *llm);

// set a pointer to a volatile abort flag (same pattern as bot_set_abort_flag)
void llm_set_abort_flag(LlmHandle *llm, volatile sig_atomic_t *flag);

// perform a single-turn chat completion.
// system_prompt may be NULL for no system message.
// user_msg is the user's input text.
// out_buf / out_cap: caller-provided buffer for the assistant reply.
// max_tokens: hard limit on generated tokens (e.g. 512).
// returns 0 on success, -1 on error (out_buf will contain a fallback message).
int llm_chat(LlmHandle *llm, const char *system_prompt,
             const char *user_msg, char *out_buf, size_t out_cap,
             int max_tokens);
